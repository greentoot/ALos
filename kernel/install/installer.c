#include "installer.h"

#include "../../driver/ata.h"
#include "../../driver/keyboard.h"
#include "../boot/bootinfo.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../tty.h"

#include <stdint.h>

#define INSTALLER_MODULE_NAME "alos-target-img"
#define INSTALLER_PART_TYPE_PERSIST 0xA0

typedef struct __attribute__((packed)) {
    uint8_t boot_flag;
    uint8_t chs_start[3];
    uint8_t type;
    uint8_t chs_end[3];
    uint8_t lba_start[4];
    uint8_t lba_count[4];
} MbrPartEntry;

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void installer_print_header(const char *title) {
    tty_clear_output();
    tty_write_color("=== ALOS Installer ===", TTY_C_INFO);
    if (title && *title) tty_write(title);
}

static void installer_format_size(uint32_t sectors, char *out, uint32_t out_sz) {
    uint32_t mib;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    mib = sectors / 2048u;
    if (mib >= 1024u) {
        ksprintf(out, "%u GiB", (unsigned)(mib / 1024u));
    } else {
        ksprintf(out, "%u MiB", (unsigned)mib);
    }
}

static void installer_print_hardware(void) {
    const BootInfo *bi = bootinfo_get();
    char line[160];
    installer_print_header("Environnement materiel detecte");
    ksprintf(line, "Memoire: %u MiB utilisables", (unsigned)(bi->mem_end_bytes / (1024u * 1024u)));
    tty_write(line);
    if (bi->has_framebuffer) {
        ksprintf(line, "Video: framebuffer %ux%u x %u bpp",
                 (unsigned)bi->fb_width,
                 (unsigned)bi->fb_height,
                 (unsigned)bi->fb_bpp);
        tty_write(line);
    } else {
        tty_write("Video: mode texte VGA");
    }
    if (bi->hypervisor_present && bi->hypervisor_vendor[0]) {
        ksprintf(line, "Hyperviseur: %s", bi->hypervisor_vendor);
        tty_write(line);
    } else {
        tty_write("Hyperviseur: aucun detecte");
    }
    tty_write("");
}

static void installer_print_partition_table(int disk_index) {
    uint8_t mbr[512];
    char line[160];
    if (ata_read_sector_device(disk_index, 0, mbr) != 0) {
        tty_write("Partitions: lecture MBR impossible");
        return;
    }
    if (!(mbr[510] == 0x55 && mbr[511] == 0xAA)) {
        tty_write("Partitions: aucune table MBR valide");
        return;
    }
    tty_write("Partitions existantes :");
    for (int i = 0; i < 4; i++) {
        const MbrPartEntry *e = (const MbrPartEntry*)&mbr[446 + i * 16];
        char size_buf[32];
        if (e->type == 0 || rd32(e->lba_count) == 0) continue;
        installer_format_size(rd32(e->lba_count), size_buf, sizeof(size_buf));
        ksprintf(line, "  p%d: type=%02x start=%u size=%s%s",
                 i + 1,
                 (unsigned)e->type,
                 (unsigned)rd32(e->lba_start),
                 size_buf,
                 (e->boot_flag == 0x80) ? " boot" : "");
        tty_write(line);
    }
}

static int installer_disk_candidates(int *out_indices, int max_indices) {
    int n = 0;
    for (int i = 0; i < ATA_MAX_DEVICES && n < max_indices; i++) {
        const AtaDeviceInfo *dev = ata_get_device(i);
        if (!dev || dev->atapi) continue;
        out_indices[n++] = i;
    }
    return n;
}

void installer_list_disks(void) {
    char line[160];
    int indices[ATA_MAX_DEVICES];
    int count;

    ata_init();
    installer_print_hardware();
    count = installer_disk_candidates(indices, ATA_MAX_DEVICES);
    if (count == 0) {
        tty_write_color("Aucun disque ATA/IDE installable detecte.", TTY_C_WARN);
        tty_write("Limite actuelle: l'installateur gere surtout les disques IDE/compatibles BIOS.");
        return;
    }

    tty_write_color("Disques detectes :", TTY_C_INFO);
    for (int i = 0; i < count; i++) {
        const AtaDeviceInfo *dev = ata_get_device(indices[i]);
        char size_buf[32];
        installer_format_size(dev->sectors, size_buf, sizeof(size_buf));
        ksprintf(line, "  [%d] hd%d %s/%s  %s  %s",
                 i,
                 indices[i],
                 dev->channel ? "secondary" : "primary",
                 dev->drive ? "slave" : "master",
                 size_buf,
                 dev->model[0] ? dev->model : "(modele inconnu)");
        tty_write(line);
    }
}

int installer_payload_available(void) {
    uint32_t payload_size = 0;
    return bootinfo_find_module(INSTALLER_MODULE_NAME, &payload_size) != 0 && payload_size >= 512u;
}

static int installer_expand_persist_partition(int disk_index, uint32_t total_sectors) {
    uint8_t mbr[512];
    if (ata_read_sector_device(disk_index, 0, mbr) != 0) return -1;
    if (!(mbr[510] == 0x55 && mbr[511] == 0xAA)) return -1;
    for (int i = 0; i < 4; i++) {
        uint8_t *e = &mbr[446 + i * 16];
        uint8_t type = e[4];
        uint32_t start = rd32(&e[8]);
        if (type != INSTALLER_PART_TYPE_PERSIST || start == 0 || start >= total_sectors) continue;
        wr32(&e[12], total_sectors - start);
        return ata_write_sector_device(disk_index, 0, mbr);
    }
    return -1;
}

static int installer_write_payload(int disk_index, char *msg, uint32_t msg_sz) {
    const void *payload;
    uint32_t payload_bytes = 0;
    uint32_t payload_sectors;
    const AtaDeviceInfo *dev = ata_get_device(disk_index);
    const uint8_t *src;
    const uint32_t chunk_sectors = 32;
    uint8_t scratch[chunk_sectors * 512u];

    if (msg && msg_sz) msg[0] = '\0';
    if (!dev || dev->atapi) {
        if (msg && msg_sz) ksprintf(msg, "Disque cible invalide.");
        return -1;
    }

    payload = bootinfo_find_module(INSTALLER_MODULE_NAME, &payload_bytes);
    if (!payload || payload_bytes < 512u) {
        if (msg && msg_sz) ksprintf(msg, "Image d'installation absente. Demarre via l'entree 'ALOS Installer' de l'ISO.");
        return -1;
    }

    payload_sectors = (payload_bytes + 511u) / 512u;
    if (dev->sectors && payload_sectors > dev->sectors) {
        if (msg && msg_sz) {
            ksprintf(msg, "Disque trop petit: cible=%u secteurs, image=%u secteurs.",
                     (unsigned)dev->sectors,
                     (unsigned)payload_sectors);
        }
        return -1;
    }

    src = (const uint8_t*)payload;
    for (uint32_t lba = 0; lba < payload_sectors; lba += chunk_sectors) {
        uint32_t todo = payload_sectors - lba;
        uint32_t percent;
        char line[128];
        if (todo > chunk_sectors) todo = chunk_sectors;
        kmemset(scratch, 0, sizeof(scratch));
        kmemcpy(scratch, src + lba * 512u, todo * 512u);
        if (ata_write_sectors_device(disk_index, lba, todo, scratch) != 0) {
            if (msg && msg_sz) ksprintf(msg, "Ecriture disque echouee au LBA %u.", (unsigned)lba);
            return -1;
        }
        percent = (uint32_t)(((uint64_t)(lba + todo) * 100u) / payload_sectors);
        tty_clear_output();
        tty_write_color("=== ALOS Installer ===", TTY_C_INFO);
        tty_write("Installation en cours...");
        ksprintf(line, "Disque cible: hd%d  |  progression: %u%%", disk_index, (unsigned)percent);
        tty_write(line);
        tty_write("Ecriture de l'image systeme + ajustement de la partition persistante.");
    }

    if (installer_expand_persist_partition(disk_index, dev->sectors) != 0) {
        if (msg && msg_sz) {
            ksprintf(msg, "ALOS installe, mais l'extension de la partition persistante a echoue.");
        }
        return 1;
    }

    if (msg && msg_sz) {
        ksprintf(msg, "Installation terminee sur hd%d. Tu peux redemarrer sans l'ISO/USB.", disk_index);
    }
    return 0;
}

void installer_run_ui(void) {
    int indices[ATA_MAX_DEVICES];
    int count;
    int sel = 0;
    int confirm = 0;
    char final_msg[160];

    ata_init();
    keyboard_clear_buffer();
    installer_print_hardware();

    if (!installer_payload_available()) {
        tty_write_color("Image d'installation absente.", TTY_C_WARN);
        tty_write("Demarre l'ISO via l'entree GRUB 'ALOS Installer' ou utilise 'make iso-installer'.");
        return;
    }

    count = installer_disk_candidates(indices, ATA_MAX_DEVICES);
    if (count == 0) {
        tty_write_color("Aucun disque ATA/IDE installable detecte.", TTY_C_WARN);
        tty_write("ALOS sait installer via le controleur IDE/ATA legacy pour le moment.");
        return;
    }

    while (1) {
        char line[192];
        installer_print_hardware();
        tty_write_color("Selectionne le disque cible d'installation :", TTY_C_INFO);
        for (int i = 0; i < count; i++) {
            const AtaDeviceInfo *dev = ata_get_device(indices[i]);
            char size_buf[32];
            installer_format_size(dev->sectors, size_buf, sizeof(size_buf));
            ksprintf(line, "%c [%d] hd%d %s/%s  %s  %s",
                     (i == sel) ? '>' : ' ',
                     i,
                     indices[i],
                     dev->channel ? "secondary" : "primary",
                     dev->drive ? "slave" : "master",
                     size_buf,
                     dev->model[0] ? dev->model : "(modele inconnu)");
            tty_write(line);
        }

        tty_write("");
        installer_print_partition_table(indices[sel]);
        tty_write("");
        tty_write_color("Attention: cette premiere version ecrase le disque entier selectionne.", TTY_C_WARN);
        tty_write("Le boot ALOS sera installe sur le disque, puis la partition persistante sera etendue sur le reste de l'espace.");
        if (!confirm) {
            tty_write("Fleches haut/bas: choisir   Entree: armer l'installation   Echap: retour shell");
        } else {
            tty_write_color("Confirmation armee: appuie sur Y pour installer, N pour annuler.", TTY_C_ERR);
        }

        while (!keyboard_available()) {
            __asm__ volatile ("hlt");
        }

        {
            uint8_t key = keyboard_getkey();
            if (key == KEY_ESCAPE || key == KEY_F1) {
                tty_write("Installateur quitte.");
                return;
            }
            if (!confirm && key == KEY_UP) {
                if (sel > 0) sel--;
                continue;
            }
            if (!confirm && key == KEY_DOWN) {
                if (sel + 1 < count) sel++;
                continue;
            }
            if (!confirm && key == KEY_ENTER) {
                confirm = 1;
                continue;
            }
            if (confirm && (key == 'n' || key == 'N' || key == KEY_BACKSPACE)) {
                confirm = 0;
                continue;
            }
            if (confirm && (key == 'y' || key == 'Y')) {
                int rc = installer_write_payload(indices[sel], final_msg, sizeof(final_msg));
                if (rc == 0) tty_write_color(final_msg, TTY_C_OK);
                else if (rc > 0) tty_write_color(final_msg, TTY_C_WARN);
                else tty_write_color(final_msg, TTY_C_ERR);
                tty_write("Appuie sur Echap pour revenir au shell.");
                while (1) {
                    uint8_t key2;
                    while (!keyboard_available()) {
                        __asm__ volatile ("hlt");
                    }
                    key2 = keyboard_getkey();
                    if (key2 == KEY_ESCAPE || key2 == KEY_F1 || key2 == KEY_ENTER) {
                        return;
                    }
                }
            }
        }
    }
}
