#include "gba_mgba.h"

#include "../../driver/gfx.h"
#include "../../driver/keyboard.h"
#include "../../driver/timer.h"
#include "../../driver/vga.h"
#include "../tty.h"
#include "../memory/heap.h"
#include "../fs/persist.h"
#include "../fs/ramfs.h"
#include "../lib/string.h"
#include "../lib/kprintf.h"
#include "../jack/vm_store.h"

#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/interface.h>
#include <mgba/gba/interface.h>
#include <mgba-util/vfs.h>
#include <mgba-util/image.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/input.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/input.h>

#define MGBA_DEFAULT_GBA_W 240
#define MGBA_DEFAULT_GBA_H 160
#define MGBA_DEFAULT_GB_W  160
#define MGBA_DEFAULT_GB_H  144
#define MGBA_FB_STRIDE     256
#define MGBA_FB_MAX_H      256
#define OUT_W 512
#define OUT_H 256
#define GBA_BIOS_SIZE 16384
#define GBA_SAVE_SLOTS 8
#define GBA_SAVE_NAME_MAX 64
#define GBA_SAVE_MAX (256 * 1024)

static uint8_t g_gba_canvas[OUT_W * OUT_H];
static int g_gba_palette_ready = 0;
static char g_gba_last_diag[160];

typedef struct {
    uint8_t used;
    char name[GBA_SAVE_NAME_MAX];
    void *data;
    uint32_t size;
} GbaSaveSlot;

static GbaSaveSlot g_saves[GBA_SAVE_SLOTS];

static void gba_set_err(char *errbuf, uint32_t errbuf_size, const char *msg) {
    if (!errbuf || errbuf_size == 0) return;
    kstrncpy(errbuf, msg ? msg : "unknown error", errbuf_size - 1);
    errbuf[errbuf_size - 1] = '\0';
}

static int gba_has_suffix(const char *s, const char *suffix) {
    uint32_t ls, lf;
    if (!s || !suffix) return 0;
    ls = kstrlen(s);
    lf = kstrlen(suffix);
    if (ls < lf) return 0;
    return kstrcmp(s + (ls - lf), suffix) == 0;
}

static const char *gba_platform_name(enum mPlatform platform) {
    switch (platform) {
        case mPLATFORM_GBA: return "GBA";
        case mPLATFORM_GB:  return "GB";
        default: return "ROM";
    }
}

static const char *gba_gb_model_name(enum GBModel model) {
    switch (model) {
        case GB_MODEL_DMG: return "DMG";
        case GB_MODEL_MGB: return "MGB";
        case GB_MODEL_SGB: return "SGB";
        case GB_MODEL_SGB2: return "SGB2";
        case GB_MODEL_CGB: return "CGB";
        case GB_MODEL_AGB: return "AGB";
        case GB_MODEL_SCGB: return "SCGB";
        default: return "AUTO";
    }
}

static const char *gba_find_blob_candidates(const char *const *vm_names,
                                            const char *const *ramfs_names,
                                            uint32_t *out_size,
                                            const char **out_name) {
    if (vm_names) {
        for (int i = 0; vm_names[i]; i++) {
            uint32_t sz = 0;
            const char *data = vm_store_find(vm_names[i], &sz);
            if (!data || sz == 0) continue;
            if (out_size) *out_size = sz;
            if (out_name) *out_name = vm_names[i];
            return data;
        }
    }

    if (ramfs_names) {
        for (int i = 0; ramfs_names[i]; i++) {
            RamFSNode *n = ramfs_find(ramfs_names[i]);
            if (!n || ramfs_is_dir(n) || n->size == 0) continue;
            if (out_size) *out_size = n->size;
            if (out_name) *out_name = ramfs_names[i];
            return ramfs_data(n);
        }
    }

    if (out_size) *out_size = 0;
    if (out_name) *out_name = 0;
    return 0;
}

static const char *gba_find_bios_blob(enum mPlatform platform,
                                      const char *rom_name,
                                      uint32_t *out_size,
                                      const char **out_name) {
    static const char *kVmStoreGba[] = {
        "gba_bios.bin",
        "bios/gba_bios.bin",
        "GBA_BIOS.BIN",
        "GBA_BIOS.bin",
        0
    };
    static const char *kRamFsGba[] = {
        "/gba_bios.bin",
        "/bios/gba_bios.bin",
        0
    };
    static const char *kVmStoreGb[] = {
        "gb_bios.bin",
        "bios/gb/gb_bios.bin",
        "dmg0_rom.bin",
        "bios/gb/dmg0_rom.bin",
        0
    };
    static const char *kRamFsGb[] = {
        "/gb_bios.bin",
        "/bios/gb/gb_bios.bin",
        "/dmg0_rom.bin",
        "/bios/gb/dmg0_rom.bin",
        0
    };
    static const char *kVmStoreGbc[] = {
        "gbc_bios.bin",
        "bios/gbc/gbc_bios.bin",
        "gb_bios.bin",
        "bios/gb/gb_bios.bin",
        "dmg0_rom.bin",
        "bios/gb/dmg0_rom.bin",
        0
    };
    static const char *kRamFsGbc[] = {
        "/gbc_bios.bin",
        "/bios/gbc/gbc_bios.bin",
        "/gb_bios.bin",
        "/bios/gb/gb_bios.bin",
        "/dmg0_rom.bin",
        "/bios/gb/dmg0_rom.bin",
        0
    };

    if (platform == mPLATFORM_GBA) {
        return gba_find_blob_candidates(kVmStoreGba, kRamFsGba, out_size, out_name);
    }
    if (platform == mPLATFORM_GB) {
        if (gba_has_suffix(rom_name, ".gbc")) {
            return gba_find_blob_candidates(kVmStoreGbc, kRamFsGbc, out_size, out_name);
        }
        return gba_find_blob_candidates(kVmStoreGb, kRamFsGb, out_size, out_name);
    }
    if (out_size) *out_size = 0;
    if (out_name) *out_name = 0;
    return 0;
}

int gba_mgba_has_bios_for_rom(const char *rom_name, uint32_t *out_size) {
    enum mPlatform platform = mPLATFORM_NONE;
    uint32_t sz = 0;

    if (gba_has_suffix(rom_name, ".gba")) {
        platform = mPLATFORM_GBA;
    } else if (gba_has_suffix(rom_name, ".gb") || gba_has_suffix(rom_name, ".gbc")) {
        platform = mPLATFORM_GB;
    } else {
        if (out_size) *out_size = 0;
        return 0;
    }

    if (!gba_find_bios_blob(platform, rom_name, &sz, 0)) {
        if (out_size) *out_size = 0;
        return 0;
    }
    if (out_size) *out_size = sz;
    return 1;
}

static void gba_setup_palette_332(void) {
    if (g_gba_palette_ready) return;
    for (int i = 0; i < 256; i++) {
        uint8_t r3 = (uint8_t)((i >> 5) & 0x7);
        uint8_t g3 = (uint8_t)((i >> 2) & 0x7);
        uint8_t b2 = (uint8_t)(i & 0x3);
        uint8_t r6 = (uint8_t)(r3 * 9);
        uint8_t g6 = (uint8_t)(g3 * 9);
        uint8_t b6 = (uint8_t)(b2 * 21);
        gfx_set_palette((uint8_t)i, r6, g6, b6);
    }
    g_gba_palette_ready = 1;
}

static uint32_t gba_crc32(const void *data, uint32_t size) {
    const uint8_t *p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < size; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = (uint32_t)-(int)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static const char *gba_basename(const char *path) {
    const char *p = path ? path : "rom";
    const char *base = p;
    while (*p) {
        if (*p == '/') base = p + 1;
        p++;
    }
    return base;
}

static void gba_make_save_key(const char *rom_name, char *out, uint32_t outsz) {
    const char *base = gba_basename(rom_name);
    uint32_t n = 0;
    while (base[n] && n + 1 < outsz) {
        out[n] = base[n];
        n++;
    }
    out[n] = '\0';
    if (n >= 4 && out[n - 4] == '.' && out[n - 3] == 'g' && out[n - 2] == 'b' && out[n - 1] == 'a') out[n - 4] = '\0';
    if (n >= 4 && out[n - 4] == '.' && out[n - 3] == 'g' && out[n - 2] == 'b' && out[n - 1] == 'c') out[n - 4] = '\0';
    if (n >= 3 && out[n - 3] == '.' && out[n - 2] == 'g' && out[n - 1] == 'b') out[n - 3] = '\0';
}

static GbaSaveSlot *gba_find_save_slot(const char *key) {
    for (int i = 0; i < GBA_SAVE_SLOTS; i++) {
        if (!g_saves[i].used) continue;
        if (kstrcmp(g_saves[i].name, key) == 0) return &g_saves[i];
    }
    return 0;
}

static GbaSaveSlot *gba_take_save_slot(const char *key) {
    GbaSaveSlot *slot = gba_find_save_slot(key);
    if (slot) return slot;
    for (int i = 0; i < GBA_SAVE_SLOTS; i++) {
        if (!g_saves[i].used) {
            g_saves[i].used = 1;
            g_saves[i].data = 0;
            g_saves[i].size = 0;
            kstrncpy(g_saves[i].name, key, GBA_SAVE_NAME_MAX - 1);
            g_saves[i].name[GBA_SAVE_NAME_MAX - 1] = '\0';
            return &g_saves[i];
        }
    }
    return 0;
}

void gba_mgba_save_clear_all(void) {
    for (int i = 0; i < GBA_SAVE_SLOTS; i++) {
        if (g_saves[i].data) free(g_saves[i].data);
        g_saves[i].used = 0;
        g_saves[i].name[0] = '\0';
        g_saves[i].data = 0;
        g_saves[i].size = 0;
    }
}

int gba_mgba_save_count(void) {
    int n = 0;
    for (int i = 0; i < GBA_SAVE_SLOTS; i++) {
        if (g_saves[i].used && g_saves[i].data && g_saves[i].size) n++;
    }
    return n;
}

int gba_mgba_save_get(int index, const char **name, const void **data, uint32_t *size) {
    int k = 0;
    for (int i = 0; i < GBA_SAVE_SLOTS; i++) {
        if (!(g_saves[i].used && g_saves[i].data && g_saves[i].size)) continue;
        if (k == index) {
            if (name) *name = g_saves[i].name;
            if (data) *data = g_saves[i].data;
            if (size) *size = g_saves[i].size;
            return 0;
        }
        k++;
    }
    return -1;
}

int gba_mgba_save_set(const char *name, const void *data, uint32_t size) {
    GbaSaveSlot *slot;
    void *copy;
    if (!name || !*name || !data || size == 0 || size > GBA_SAVE_MAX) return -1;
    slot = gba_take_save_slot(name);
    if (!slot) return -1;
    copy = malloc(size);
    if (!copy) return -1;
    kmemcpy(copy, data, size);
    if (slot->data) free(slot->data);
    slot->data = copy;
    slot->size = size;
    persist_mark_dirty();
    return 0;
}

static void gba_try_restore_save(struct mCore *core, const char *rom_name) {
    char key[GBA_SAVE_NAME_MAX];
    GbaSaveSlot *slot;
    if (!core || !core->savedataRestore) return;
    gba_make_save_key(rom_name, key, sizeof(key));
    slot = gba_find_save_slot(key);
    if (!slot || !slot->data || slot->size == 0) return;
    core->savedataRestore(core, slot->data, slot->size, true);
}

static void gba_try_store_save(struct mCore *core, const char *rom_name) {
    char key[GBA_SAVE_NAME_MAX];
    GbaSaveSlot *slot;
    void *clone = 0;
    size_t sz = 0;

    if (!core || !core->savedataClone) return;

    sz = core->savedataClone(core, &clone);
    if (!clone || sz == 0 || sz > GBA_SAVE_MAX) {
        if (clone) free(clone);
        return;
    }

    gba_make_save_key(rom_name, key, sizeof(key));
    slot = gba_take_save_slot(key);
    if (!slot) {
        free(clone);
        return;
    }

    if (slot->data) free(slot->data);
    slot->data = clone;
    slot->size = (uint32_t)sz;
    persist_mark_dirty();
}

static inline uint32_t gba_key_bit_for_platform(uint8_t key, enum mPlatform platform) {
    if (platform == mPLATFORM_GB) {
        switch (key) {
            case KEY_RIGHT: return (1u << GB_KEY_RIGHT);
            case KEY_LEFT:  return (1u << GB_KEY_LEFT);
            case KEY_UP:    return (1u << GB_KEY_UP);
            case KEY_DOWN:  return (1u << GB_KEY_DOWN);
            case KEY_ENTER: return (1u << GB_KEY_START);
            case KEY_BACKSPACE: return (1u << GB_KEY_SELECT);
            case 'x': case 'X': case 'k': case 'K': return (1u << GB_KEY_A);
            case 'z': case 'Z': case 'j': case 'J': return (1u << GB_KEY_B);
            default: return 0u;
        }
    }
    switch (key) {
        case KEY_RIGHT: return (1u << GBA_KEY_RIGHT);
        case KEY_LEFT:  return (1u << GBA_KEY_LEFT);
        case KEY_UP:    return (1u << GBA_KEY_UP);
        case KEY_DOWN:  return (1u << GBA_KEY_DOWN);
        case KEY_ENTER: return (1u << GBA_KEY_START);
        case KEY_BACKSPACE: return (1u << GBA_KEY_SELECT);
        case 'x': case 'X': case 'k': case 'K': return (1u << GBA_KEY_A);
        case 'z': case 'Z': case 'j': case 'J': return (1u << GBA_KEY_B);
        case 'a': case 'A': case 'q': case 'Q': return (1u << GBA_KEY_L);
        case 's': case 'S': return (1u << GBA_KEY_R);
        default: return 0u;
    }
}

static uint32_t gba_sample_keys(enum mPlatform platform, int *quit, int *toggle_speed) {
    uint32_t keys = 0;
    uint8_t held = keyboard_current_key();
    keys |= gba_key_bit_for_platform(held, platform);

    while (keyboard_available()) {
        uint8_t k = keyboard_getkey();
        if (k == KEY_ESCAPE) {
            if (quit) *quit = 1;
            continue;
        }
        if (k == KEY_F2) {
            if (toggle_speed) *toggle_speed = 1;
            continue;
        }
        keys |= gba_key_bit_for_platform(k, platform);
    }
    return keys;
}

static void gba_blit_frame(const mColor *src, int src_w, int src_h, int src_stride) {
    const int ox = (OUT_W - src_w) / 2;
    const int oy = (OUT_H - src_h) / 2;

    kmemset(g_gba_canvas, 0, OUT_W * OUT_H);

    for (int y = 0; y < src_h; y++) {
        uint8_t *dst = &g_gba_canvas[(oy + y) * OUT_W + ox];
        const mColor *row = &src[y * src_stride];
        for (int x = 0; x < src_w; x++) {
            uint32_t c = (uint32_t)row[x];
            uint8_t r = (uint8_t)(c & 0xFFu);
            uint8_t g = (uint8_t)((c >> 8) & 0xFFu);
            uint8_t b = (uint8_t)((c >> 16) & 0xFFu);
            dst[x] = (uint8_t)(((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6));
        }
    }
}

/* Deterministic startup without BIOS:
 * clear RAM-like blocks that are undefined at power-on (prevents mosaic boots). */
static void gba_clear_poweron_like_memory(struct mCore *core) {
    const struct mCoreMemoryBlock *blocks = 0;
    size_t count = 0;

    if (!core || !core->listMemoryBlocks || !core->getMemoryBlock) return;
    count = core->listMemoryBlocks(core, &blocks);
    if (!blocks || count == 0) return;

    for (size_t i = 0; i < count; i++) {
        const struct mCoreMemoryBlock *b = &blocks[i];
        void *ptr = 0;
        size_t sz = 0;
        int clear = 0;

        switch (b->start) {
            case 0x02000000u: /* EWRAM */
            case 0x03000000u: /* IWRAM */
            case 0x05000000u: /* Palette RAM */
            case 0x06000000u: /* VRAM */
            case 0x07000000u: /* OAM */
                clear = 1;
                break;
            default:
                break;
        }
        if (!clear) continue;

        ptr = core->getMemoryBlock(core, b->id, &sz);
        if (!ptr || sz == 0 || sz > (2u * 1024u * 1024u)) continue;
        kmemset(ptr, 0, (uint32_t)sz);
    }
}

static int gba_force_known_override(struct mCore *core,
                                    const void *rom_data,
                                    uint32_t rom_size,
                                    char out_code[5],
                                    uint32_t *out_crc) {
    struct GBACartridgeOverride override;
    const char *code = 0;
    uint32_t crc = 0;

    if (out_code) {
        out_code[0] = '?';
        out_code[1] = '?';
        out_code[2] = '?';
        out_code[3] = '?';
        out_code[4] = '\0';
    }
    if (!core || !core->setOverride || !rom_data || rom_size < 0xB0) return 0;

    code = (const char *)rom_data + 0xAC;
    crc = gba_crc32(rom_data, rom_size);
    if (out_crc) *out_crc = crc;
    if (out_code) {
        out_code[0] = code[0];
        out_code[1] = code[1];
        out_code[2] = code[2];
        out_code[3] = code[3];
        out_code[4] = '\0';
    }

    kmemset(&override, 0, sizeof(override));
    override.id[0] = code[0];
    override.id[1] = code[1];
    override.id[2] = code[2];
    override.id[3] = code[3];
    override.idleLoop = GBA_IDLE_LOOP_NONE;
    override.savetype = GBA_SAVEDATA_AUTODETECT;
    override.hardware = HW_NO_OVERRIDE;

    if (code[0] == 'B' && code[1] == 'P' && code[2] == 'E') {
        override.savetype = GBA_SAVEDATA_FLASH1M;
        override.hardware = HW_RTC;
        override.idleLoop = 0x80008C6u;
    } else if (code[0] == 'B' && code[1] == 'P' &&
               (code[2] == 'R' || code[2] == 'G')) {
        override.savetype = GBA_SAVEDATA_FLASH1M;
        override.hardware = HW_NONE;
        override.idleLoop = GBA_IDLE_LOOP_NONE;
    } else {
        return 0;
    }

    core->setOverride(core, &override);
    return 1;
}

int gba_mgba_run_rom(const char *rom_name,
                     const void *rom_data,
                     uint32_t rom_size,
                     char *errbuf,
                     uint32_t errbuf_size) {
    struct VFile *rom_vf = 0;
    struct VFile *bios_vf = 0;
    struct mCore *core = 0;
    mColor *fb = 0;
    const char *bios_blob = 0;
    const char *bios_name = 0;
    uint32_t bios_size = 0;
    int bios_loaded = 0;
    uint32_t next_ms;
    int quit = 0;
    int cfg_inited = 0;
    int rom_loaded = 0;
    int graphics_active = 0;
    int want_bios = 0;
    int override_forced = 0;
    int ret = -1;
    int fast_x2 = 0;
    char rom_code[5];
    uint32_t rom_crc = 0;
    uint32_t last_toggle_ms = 0;
    enum mPlatform platform = mPLATFORM_NONE;
    unsigned frame_w = 0;
    unsigned frame_h = 0;

    g_gba_last_diag[0] = '\0';

    if (!rom_data || rom_size < 192) {
        gba_set_err(errbuf, errbuf_size, "ROM vide ou invalide");
        return -1;
    }

    rom_vf = VFileFromConstMemory(rom_data, rom_size);
    if (!rom_vf) {
        gba_set_err(errbuf, errbuf_size, "VFileFromConstMemory a echoue");
        return -2;
    }

    core = mCoreFindVF(rom_vf);
    if (!core) {
        rom_vf->close(rom_vf);
        gba_set_err(errbuf, errbuf_size, "format ROM non reconnu");
        return -3;
    }
    platform = core->platform ? core->platform(core) : mPLATFORM_NONE;

    if (!core->init(core)) {
        gba_set_err(errbuf, errbuf_size, "init mGBA a echoue");
        ret = -4;
        goto done;
    }

    mCoreConfigInit(&core->config, "alos");
    cfg_inited = 1;
    mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "remove");
    if (platform == mPLATFORM_GB) {
        if (gba_has_suffix(rom_name, ".gbc")) {
            mCoreConfigSetDefaultValue(&core->config, "cgb.model", "CGB");
        } else {
            mCoreConfigSetDefaultValue(&core->config, "gb.model", "DMG");
        }
    }
    bios_blob = gba_find_bios_blob(platform, rom_name, &bios_size, &bios_name);
    if (platform == mPLATFORM_GBA) {
        want_bios = (bios_blob && bios_size == GBA_BIOS_SIZE) ? 1 : 0;
    } else if (platform == mPLATFORM_GB) {
        want_bios = bios_blob && bios_size > 0;
    } else {
        want_bios = 0;
    }
    if (want_bios) {
        mCoreConfigSetDefaultValue(&core->config, "useBios", "1");
        mCoreConfigSetDefaultValue(&core->config, "skipBios", "0");
    } else {
        mCoreConfigSetDefaultValue(&core->config, "useBios", "0");
        mCoreConfigSetDefaultValue(&core->config, "skipBios", "1");
    }
    mCoreLoadConfig(core);
    /* Force explicit behavior: no valid BIOS => always skip BIOS boot path. */
    core->opts.useBios = want_bios ? true : false;
    core->opts.skipBios = want_bios ? false : true;

    if (want_bios && core->loadBIOS) {
        bios_vf = VFileFromConstMemory(bios_blob, bios_size);
        if (bios_vf && core->loadBIOS(core, bios_vf, 0)) {
            bios_loaded = 1;
            bios_vf = 0; /* ownership transferred to core */
            if (core->selectBIOS) core->selectBIOS(core, 0);
        } else {
            char m[128];
            if (bios_vf) {
                bios_vf->close(bios_vf);
                bios_vf = 0;
            }
            core->opts.useBios = false;
            core->opts.skipBios = true;
            ksprintf(m, "BIOS detecte (%s) mais loadBIOS a echoue, fallback sans BIOS.",
                     bios_name ? bios_name : "gba_bios.bin");
            tty_write(m);
        }
    } else if (platform == mPLATFORM_GBA && bios_blob && bios_size != GBA_BIOS_SIZE) {
        char m[96];
        ksprintf(m, "BIOS ignore (%s: %u bytes)", bios_name ? bios_name : "gba_bios.bin", (unsigned)bios_size);
        kprintf("%s\n", m);
    }

    fb = (mColor*)kmalloc((uint32_t)(MGBA_FB_STRIDE * MGBA_FB_MAX_H * sizeof(mColor)));
    if (!fb) {
        gba_set_err(errbuf, errbuf_size, "OOM framebuffer");
        ret = -5;
        goto done;
    }
    kmemset(fb, 0, (uint32_t)(MGBA_FB_STRIDE * MGBA_FB_MAX_H * sizeof(mColor)));
    core->setVideoBuffer(core, fb, MGBA_FB_STRIDE);

    if (!core->loadROM(core, rom_vf)) {
        gba_set_err(errbuf, errbuf_size, "loadROM a echoue");
        ret = -6;
        goto done;
    }
    rom_vf = 0; /* ownership transferred to core */
    rom_loaded = 1;
    if (platform == mPLATFORM_GBA) {
        override_forced = gba_force_known_override(core, rom_data, rom_size, rom_code, &rom_crc);
    } else {
        rom_code[0] = '\0';
        rom_crc = gba_crc32(rom_data, rom_size);
    }

    core->reset(core);
    if (platform == mPLATFORM_GBA && !bios_loaded) {
        gba_clear_poweron_like_memory(core);
    }
    gba_try_restore_save(core, rom_name);

    if (core->currentVideoSize) {
        core->currentVideoSize(core, &frame_w, &frame_h);
    }
    if (!frame_w || !frame_h) {
        if (platform == mPLATFORM_GB) {
            frame_w = MGBA_DEFAULT_GB_W;
            frame_h = MGBA_DEFAULT_GB_H;
        } else {
            frame_w = MGBA_DEFAULT_GBA_W;
            frame_h = MGBA_DEFAULT_GBA_H;
        }
    }
    if (frame_w > MGBA_FB_STRIDE || frame_h > MGBA_FB_MAX_H) {
        gba_set_err(errbuf, errbuf_size, "dimensions video non supportees");
        ret = -7;
        goto done;
    }

    if (platform == mPLATFORM_GBA && core->board) {
        struct GBA *gba = (struct GBA*)core->board;
        char diag[160];
        ksprintf(diag,
                 "GBA cfg: code=%s crc=%08x save=%d hw=%x idle=%08x bios=%d ov=%d",
                 rom_code,
                 (unsigned)rom_crc,
                 gba->memory.savedata.type,
                 (unsigned)gba->memory.hw.devices,
                 (unsigned)gba->idleLoop,
                 bios_loaded ? 1 : 0,
                 override_forced ? 1 : 0);
        kstrncpy(g_gba_last_diag, diag, sizeof(g_gba_last_diag) - 1);
        g_gba_last_diag[sizeof(g_gba_last_diag) - 1] = '\0';
        tty_write(diag);
    } else if (platform == mPLATFORM_GB && core->board) {
        struct GB *gb = (struct GB*)core->board;
        char diag[160];
        ksprintf(diag,
                 "GB cfg: model=%s crc=%08x bios=%d",
                 gba_gb_model_name(gb->model),
                 (unsigned)rom_crc,
                 bios_loaded ? 1 : 0);
        kstrncpy(g_gba_last_diag, diag, sizeof(g_gba_last_diag) - 1);
        g_gba_last_diag[sizeof(g_gba_last_diag) - 1] = '\0';
        tty_write(diag);
    }

    gfx_set_mode13();
    graphics_active = 1;
    gba_setup_palette_332();
    gfx_clear(0);

    next_ms = timer_ms() + 16;
    while (!quit) {
        int toggle_speed = 0;
        uint32_t keys = gba_sample_keys(platform, &quit, &toggle_speed);
        if (toggle_speed) {
            uint32_t now = timer_ms();
            if ((int32_t)(now - last_toggle_ms) > 150) {
                fast_x2 = !fast_x2;
                last_toggle_ms = now;
                next_ms = now + 16;
            }
        }
        core->setKeys(core, keys);
        core->runFrame(core);
        if (fast_x2 && !quit) {
            core->runFrame(core);
        }

        gba_blit_frame(fb, (int)frame_w, (int)frame_h, MGBA_FB_STRIDE);
        gfx_blit_indexed_512x256(g_gba_canvas);

        while ((int32_t)(next_ms - timer_ms()) > 0) {
            __asm__ volatile ("hlt");
        }
        next_ms += 16;
        if ((int32_t)(timer_ms() - next_ms) > 200) {
            next_ms = timer_ms() + 16;
        }
    }

    ret = 0;

done:
    if (core && rom_loaded) {
        gba_try_store_save(core, rom_name);
        core->unloadROM(core);
    }
    if (fb) kfree(fb);
    if (core && cfg_inited) mCoreConfigDeinit(&core->config);
    if (core) core->deinit(core);
    if (bios_vf) bios_vf->close(bios_vf);
    if (rom_vf) rom_vf->close(rom_vf);

    if (graphics_active) {
        gfx_restore_text_mode();
        vga_init();
        tty_init();
        if (ret == 0) {
            if (g_gba_last_diag[0]) {
                tty_write(g_gba_last_diag);
            }
            if (bios_loaded) {
                char msg[128];
                ksprintf(msg,
                         "Retour au shell ALOS (%s BIOS active, vitesse finale %s).",
                         gba_platform_name(platform),
                         fast_x2 ? "x2" : "x1");
                tty_write(msg);
            } else {
                char msg[96];
                ksprintf(msg, "Retour au shell ALOS (vitesse finale %s).", fast_x2 ? "x2" : "x1");
                tty_write(msg);
            }
        }
    }
    return ret;
}
