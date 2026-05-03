#include "nds_backend.h"
#include "nds_core.h"

#include "../fs/ramfs.h"
#include "../jack/vm_store.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../../driver/serial.h"
#include "../../driver/timer.h"

static uint32_t g_nds_host_request_id = 0;
static char g_serial_line_buf[192];
static uint32_t g_serial_line_len = 0;
static int g_nds_bridge_status = -1;
static uint32_t g_nds_bridge_probe_ms = 0;

static uint32_t nds_read_u32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static const char *nds_find_blob_candidates(const char *const *vm_names,
                                            const char *const *ramfs_names,
                                            uint32_t *out_size) {
    if (vm_names) {
        for (int i = 0; vm_names[i]; i++) {
            uint32_t sz = 0;
            const char *data = vm_store_find(vm_names[i], &sz);
            if (data && sz > 0) {
                if (out_size) *out_size = sz;
                return data;
            }
        }
    }
    if (ramfs_names) {
        for (int i = 0; ramfs_names[i]; i++) {
            RamFSNode *n = ramfs_find(ramfs_names[i]);
            if (n && !ramfs_is_dir(n) && n->size > 0) {
                if (out_size) *out_size = n->size;
                return ramfs_data(n);
            }
        }
    }
    if (out_size) *out_size = 0;
    return 0;
}

void nds_rom_info_init(NdsRomInfo *info) {
    if (!info) return;
    kmemset(info, 0, sizeof(*info));
}

void nds_backend_status_init(NdsBackendStatus *status) {
    if (!status) return;
    kmemset(status, 0, sizeof(*status));
    nds_rom_info_init(&status->rom);
}

int nds_backend_host_bridge_available(void) {
    return serial_is_ready();
}

int nds_backend_internal_ready(void) {
    return nds_core_available();
}

void nds_bridge_event_init(NdsBridgeEvent *event) {
    if (!event) return;
    kmemset(event, 0, sizeof(*event));
}

static int nds_parse_host_event_line(const char *line, NdsBridgeEvent *event) {
    const char *p;
    const char *req_start;
    uint32_t req_len = 0;
    const char *kind_start;
    uint32_t kind_len = 0;
    const char *detail_start;

    if (!line || !event) return 0;
    if (kstrncmp(line, "ALOS_HOSTEVENT|NDS|", 19) != 0) return 0;

    nds_bridge_event_init(event);

    p = line + 19;
    req_start = p;
    while (*p && *p != '|') {
        req_len++;
        p++;
    }
    if (*p != '|' || req_len == 0 || req_len >= 16) return 0;
    {
        char req_buf[16];
        kmemcpy(req_buf, req_start, req_len);
        req_buf[req_len] = '\0';
        event->request_id = (uint32_t)katoi(req_buf);
    }
    p++;
    kind_start = p;
    while (*p && *p != '|') {
        kind_len++;
        p++;
    }
    detail_start = (*p == '|') ? (p + 1) : p;

    if (kind_len == 5 && kstrncmp(kind_start, "READY", 5) == 0) {
        event->kind = NDS_BRIDGE_EVENT_READY;
    } else if (kind_len == 7 && kstrncmp(kind_start, "STARTED", 7) == 0) {
        event->kind = NDS_BRIDGE_EVENT_STARTED;
    } else if (kind_len == 6 && kstrncmp(kind_start, "CLOSED", 6) == 0) {
        event->kind = NDS_BRIDGE_EVENT_CLOSED;
    } else if (kind_len == 5 && kstrncmp(kind_start, "ERROR", 5) == 0) {
        event->kind = NDS_BRIDGE_EVENT_ERROR;
    } else {
        return 0;
    }

    if (*detail_start) {
        kstrncpy(event->detail, detail_start, sizeof(event->detail) - 1);
        event->detail[sizeof(event->detail) - 1] = '\0';
    }
    return 1;
}

int nds_backend_bridge_ready(void) {
    uint32_t start_ms;

    if (!serial_is_ready()) {
        g_nds_bridge_status = 0;
        g_nds_bridge_probe_ms = timer_ms();
        return 0;
    }

    if (g_nds_bridge_status >= 0) {
        uint32_t age = timer_ms() - g_nds_bridge_probe_ms;
        if (age < 1500U) return g_nds_bridge_status;
    }

    g_nds_bridge_probe_ms = timer_ms();
    serial_write_line("ALOS_HOSTPING|NDS");
    start_ms = timer_ms();
    while ((timer_ms() - start_ms) < 180U) {
        NdsBridgeEvent event;
        while (nds_backend_poll_bridge_event(&event)) {
            if (event.kind == NDS_BRIDGE_EVENT_READY) {
                g_nds_bridge_status = 1;
                return 1;
            }
        }
    }
    g_nds_bridge_status = 0;
    return 0;
}

int nds_backend_can_launch(void) {
    if (nds_backend_internal_ready()) return 1;
    return nds_backend_bridge_ready();
}

int nds_backend_poll_bridge_event(NdsBridgeEvent *event) {
    char c;
    NdsBridgeEvent parsed;

    if (!serial_is_ready()) return 0;
    while (serial_read_char(&c)) {
        if (c == '\r') continue;
        if (c == '\n') {
            g_serial_line_buf[g_serial_line_len] = '\0';
            g_serial_line_len = 0;
            if (nds_parse_host_event_line(g_serial_line_buf, &parsed)) {
                if (event) *event = parsed;
                return 1;
            }
            continue;
        }
        if (g_serial_line_len + 1 < sizeof(g_serial_line_buf)) {
            g_serial_line_buf[g_serial_line_len++] = c;
        } else {
            g_serial_line_len = 0;
        }
    }
    return 0;
}

int nds_rom_info_parse(const void *rom_data, uint32_t rom_size, NdsRomInfo *info) {
    const uint8_t *rom = (const uint8_t *)rom_data;
    if (!rom_data || rom_size < 0x200 || !info) return 0;

    nds_rom_info_init(info);
    kmemcpy(info->title, rom, 12);
    info->title[12] = '\0';
    kmemcpy(info->game_code, rom + 0x0C, 4);
    info->game_code[4] = '\0';
    kmemcpy(info->maker_code, rom + 0x10, 2);
    info->maker_code[2] = '\0';
    info->unit_code = rom[0x12];
    info->device_capacity = rom[0x14];

    info->arm9_rom_offset = nds_read_u32(rom + 0x20);
    info->arm9_entry = nds_read_u32(rom + 0x24);
    info->arm9_ram_address = nds_read_u32(rom + 0x28);
    info->arm9_size = nds_read_u32(rom + 0x2C);
    info->arm7_rom_offset = nds_read_u32(rom + 0x30);
    info->arm7_entry = nds_read_u32(rom + 0x34);
    info->arm7_ram_address = nds_read_u32(rom + 0x38);
    info->arm7_size = nds_read_u32(rom + 0x3C);

    if (info->arm9_rom_offset >= rom_size || info->arm7_rom_offset >= rom_size) return 0;
    if (info->arm9_size == 0 || info->arm7_size == 0) return 0;
    return 1;
}

int nds_backend_probe_assets(uint32_t *bios7_size,
                             uint32_t *bios9_size,
                             uint32_t *firmware_size,
                             uint32_t *keycfg_size) {
    static const char *kVmBios7[] = {
        "bios/nds/biosnds7.rom",
        "biosnds7.rom",
        0
    };
    static const char *kFsBios7[] = {
        "/bios/nds/biosnds7.rom",
        "/biosnds7.rom",
        0
    };
    static const char *kVmBios9[] = {
        "bios/nds/biosnds9.rom",
        "biosnds9.rom",
        0
    };
    static const char *kFsBios9[] = {
        "/bios/nds/biosnds9.rom",
        "/biosnds9.rom",
        0
    };
    static const char *kVmFw[] = {
        "bios/nds/firmware.bin",
        "firmware.bin",
        0
    };
    static const char *kFsFw[] = {
        "/bios/nds/firmware.bin",
        "/firmware.bin",
        0
    };
    static const char *kVmKey[] = {
        "bios/nds/key.cfg",
        "key.cfg",
        0
    };
    static const char *kFsKey[] = {
        "/bios/nds/key.cfg",
        "/key.cfg",
        0
    };

    int ok = 1;
    if (!nds_find_blob_candidates(kVmBios7, kFsBios7, bios7_size)) ok = 0;
    if (!nds_find_blob_candidates(kVmBios9, kFsBios9, bios9_size)) ok = 0;
    if (!nds_find_blob_candidates(kVmFw, kFsFw, firmware_size)) ok = 0;
    nds_find_blob_candidates(kVmKey, kFsKey, keycfg_size);
    return ok;
}

int nds_backend_preflight(const char *rom_name,
                          const void *rom_data,
                          uint32_t rom_size,
                          NdsBackendStatus *status,
                          char *errbuf,
                          uint32_t errbuf_size) {
    int assets_ok = 0;

    if (!errbuf || errbuf_size == 0) return 0;
    errbuf[0] = '\0';

    if (status) nds_backend_status_init(status);
    if (status) status->rom_size = rom_size;

    if (!rom_data || rom_size < 0x200) {
        kstrncpy(errbuf, "ROM NDS vide ou invalide", errbuf_size - 1);
        errbuf[errbuf_size - 1] = '\0';
        return 0;
    }

    if (status) {
        status->rom_valid = nds_rom_info_parse(rom_data, rom_size, &status->rom);
        if (!status->rom_valid) {
            kstrncpy(errbuf, "header NDS invalide", errbuf_size - 1);
            errbuf[errbuf_size - 1] = '\0';
            return 0;
        }
        assets_ok = nds_backend_probe_assets(&status->bios7_size,
                                             &status->bios9_size,
                                             &status->firmware_size,
                                             &status->keycfg_size);
        status->assets_ready = assets_ok;
        if (!assets_ok) {
            ksprintf(errbuf,
                     "preflight NDS incomplet: bios7=%u bios9=%u firmware=%u keycfg=%u",
                     (unsigned)status->bios7_size,
                     (unsigned)status->bios9_size,
                     (unsigned)status->firmware_size,
                     (unsigned)status->keycfg_size);
            return 0;
        }
    } else {
        uint32_t bios7 = 0, bios9 = 0, fw = 0, keycfg = 0;
        NdsRomInfo info;
        nds_rom_info_init(&info);
        if (!nds_rom_info_parse(rom_data, rom_size, &info)) {
            kstrncpy(errbuf, "header NDS invalide", errbuf_size - 1);
            errbuf[errbuf_size - 1] = '\0';
            return 0;
        }
        assets_ok = nds_backend_probe_assets(&bios7, &bios9, &fw, &keycfg);
        if (!assets_ok) {
            ksprintf(errbuf,
                     "preflight NDS incomplet: bios7=%u bios9=%u firmware=%u keycfg=%u",
                     (unsigned)bios7, (unsigned)bios9, (unsigned)fw, (unsigned)keycfg);
            return 0;
        }
    }

    if (nds_backend_internal_ready()) {
        ksprintf(errbuf,
                 "ROM NDS valide (%s / %s), BIOS+firmware detectes, core DS interne attendu",
                 rom_name ? rom_name : "rom.nds",
                 (status && status->rom.game_code[0]) ? status->rom.game_code : "----");
    } else if (nds_backend_bridge_ready()) {
        ksprintf(errbuf,
                 "ROM NDS valide (%s / %s), BIOS+firmware detectes, bridge DS dev disponible",
                 rom_name ? rom_name : "rom.nds",
                 (status && status->rom.game_code[0]) ? status->rom.game_code : "----");
    } else {
        ksprintf(errbuf,
                 "ROM NDS valide (%s / %s), assets OK mais core DS interne encore absent",
                 rom_name ? rom_name : "rom.nds",
                 (status && status->rom.game_code[0]) ? status->rom.game_code : "----");
    }
    return 1;
}

int nds_backend_begin_session(const char *rom_name,
                              const void *rom_data,
                              uint32_t rom_size,
                              uint32_t *out_request_id,
                              char *errbuf,
                              uint32_t errbuf_size) {
    NdsBackendStatus status;

    if (!errbuf || errbuf_size == 0) return -1;
    errbuf[0] = '\0';
    if (out_request_id) *out_request_id = 0;

    if (!nds_backend_preflight(rom_name, rom_data, rom_size, &status, errbuf, errbuf_size)) {
        return -1;
    }
    if (!nds_backend_bridge_ready()) {
        ksprintf(errbuf,
                 "ROM NDS valide (%s / %s), mais aucune passerelle hote DS n'a repondu",
                 rom_name ? rom_name : "rom.nds",
                 status.rom.game_code[0] ? status.rom.game_code : "----");
        return -4;
    }
    {
        char hostcmd[192];
        uint32_t req = ++g_nds_host_request_id;
        if (out_request_id) *out_request_id = req;
        ksprintf(hostcmd,
                 "ALOS_HOSTCMD|NDS|%u|%s",
                 (unsigned)req,
                 rom_name ? rom_name : "rom.nds");
        serial_write_line(hostcmd);
    }
    ksprintf(errbuf,
             "ROM NDS valide (%s / %s), lancement DS demande via passerelle hote",
             rom_name ? rom_name : "rom.nds",
             status.rom.game_code[0] ? status.rom.game_code : "----");
    return 0;
}

int nds_backend_wait_session(const char *rom_name,
                             const void *rom_data,
                             uint32_t rom_size,
                             char *errbuf,
                             uint32_t errbuf_size) {
    uint32_t req = 0;
    uint32_t start_ms;
    int saw_started = 0;

    if (!errbuf || errbuf_size == 0) return -1;
    if (nds_backend_internal_ready()) {
        return nds_core_run_rom(rom_name, rom_data, rom_size, errbuf, errbuf_size);
    }
    if (nds_backend_begin_session(rom_name, rom_data, rom_size, &req, errbuf, errbuf_size) != 0) {
        return -1;
    }

    start_ms = timer_ms();
    for (;;) {
        NdsBridgeEvent event;
        while (nds_backend_poll_bridge_event(&event)) {
            if (event.request_id != req) continue;
            if (event.kind == NDS_BRIDGE_EVENT_STARTED) {
                saw_started = 1;
                ksprintf(errbuf,
                         "Session DS active: %s",
                         event.detail[0] ? event.detail : "fenetre hote ouverte");
            } else if (event.kind == NDS_BRIDGE_EVENT_CLOSED) {
                ksprintf(errbuf,
                         "Session DS fermee: %s",
                         event.detail[0] ? event.detail : "retour a ALOS");
                return 0;
            } else if (event.kind == NDS_BRIDGE_EVENT_ERROR) {
                ksprintf(errbuf,
                         "Bridge DS: erreur hote (%s)",
                         event.detail[0] ? event.detail : "inconnue");
                return -2;
            }
        }

        if (!saw_started && (timer_ms() - start_ms) > 5000U) {
            kstrncpy(errbuf, "Bridge DS: aucun acquittement STARTED recu", errbuf_size - 1);
            errbuf[errbuf_size - 1] = '\0';
            return -3;
        }
        __asm__ volatile("hlt");
    }
}

int nds_backend_run_probe(const char *rom_name,
                          const void *rom_data,
                          uint32_t rom_size,
                          uint32_t frames,
                          char *errbuf,
                          uint32_t errbuf_size) {
    NdsBackendStatus status;

    if (!errbuf || errbuf_size == 0) return -1;
    errbuf[0] = '\0';

    nds_backend_status_init(&status);
    if (!nds_backend_preflight(rom_name, rom_data, rom_size, &status, errbuf, errbuf_size)) {
        return -1;
    }
    if (!nds_backend_internal_ready()) {
        kstrncpy(errbuf,
                 "ndsprobe: core DS interne inactif dans ce build (utilise make run-nds-internal)",
                 errbuf_size - 1);
        errbuf[errbuf_size - 1] = '\0';
        return -2;
    }
    if (frames == 0) frames = 1;
    return nds_core_boot_probe(rom_name, rom_data, rom_size, frames, errbuf, errbuf_size);
}
