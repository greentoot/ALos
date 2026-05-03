#include "jack_assets.h"
#include "vm_store.h"
#include "../lib/string.h"
#include "../lib/kprintf.h"
#include <stdint.h>

#include <mgba/flags.h>

#ifdef USE_PNG
#include <mgba-util/image.h>
#include <mgba-util/vfs.h>
#endif

#ifndef ALOS_HAS_GBA_BIOS
#define ALOS_HAS_GBA_BIOS 0
#endif
#ifndef ALOS_HAS_GB_BIOS
#define ALOS_HAS_GB_BIOS 0
#endif
#ifndef ALOS_HAS_GB_DMG0_BIOS
#define ALOS_HAS_GB_DMG0_BIOS 0
#endif
#ifndef ALOS_HAS_GBC_BIOS
#define ALOS_HAS_GBC_BIOS 0
#endif
#ifndef ALOS_HAS_NDS_BIOS7
#define ALOS_HAS_NDS_BIOS7 0
#endif
#ifndef ALOS_HAS_NDS_BIOS9
#define ALOS_HAS_NDS_BIOS9 0
#endif
#ifndef ALOS_HAS_NDS_FIRMWARE
#define ALOS_HAS_NDS_FIRMWARE 0
#endif
#ifndef ALOS_HAS_NDS_KEYCFG
#define ALOS_HAS_NDS_KEYCFG 0
#endif

#define JACK_COVER_PREVIEW_W   86
#define JACK_COVER_PREVIEW_H   86
#define JACK_COVER_CACHE_MAX   8
#define JACK_COVER_BG_R        244
#define JACK_COVER_BG_G        246
#define JACK_COVER_BG_B        248
#define JACK_COVER_BG_INDEX    2

typedef struct {
    char title[VM_NAME_MAX];
    uint8_t pixels[JACK_COVER_PREVIEW_W * JACK_COVER_PREVIEW_H];
    uint8_t state; /* 0=empty, 1=ready, 2=missing/failed */
} JackCoverPreviewCache;

static JackCoverPreviewCache g_cover_cache[JACK_COVER_CACHE_MAX];

/* Generated at build time. Always present, even if no ROM is found. */
void jack_assets_auto_mount(void);

#if ALOS_HAS_GBA_BIOS
extern const uint8_t _binary_assets_gba_bios_bin_start[];
extern const uint8_t _binary_assets_gba_bios_bin_end[];
#endif
#if ALOS_HAS_GB_BIOS
extern const uint8_t _binary_assets_gb_bios_bin_start[];
extern const uint8_t _binary_assets_gb_bios_bin_end[];
#endif
#if ALOS_HAS_GB_DMG0_BIOS
extern const uint8_t _binary_assets_dmg0_rom_bin_start[];
extern const uint8_t _binary_assets_dmg0_rom_bin_end[];
#endif
#if ALOS_HAS_GBC_BIOS
extern const uint8_t _binary_assets_gbc_bios_bin_start[];
extern const uint8_t _binary_assets_gbc_bios_bin_end[];
#endif
#if ALOS_HAS_NDS_BIOS7
extern const uint8_t _binary_assets_biosnds7_rom_start[];
extern const uint8_t _binary_assets_biosnds7_rom_end[];
#endif
#if ALOS_HAS_NDS_BIOS9
extern const uint8_t _binary_assets_biosnds9_rom_start[];
extern const uint8_t _binary_assets_biosnds9_rom_end[];
#endif
#if ALOS_HAS_NDS_FIRMWARE
extern const uint8_t _binary_assets_firmware_bin_start[];
extern const uint8_t _binary_assets_firmware_bin_end[];
#endif
#if ALOS_HAS_NDS_KEYCFG
extern const uint8_t _binary_assets_key_cfg_start[];
extern const uint8_t _binary_assets_key_cfg_end[];
#endif

static void jack_assets_fill_bg(uint8_t *pixels) {
    for (int i = 0; i < JACK_COVER_PREVIEW_W * JACK_COVER_PREVIEW_H; i++) {
        pixels[i] = JACK_COVER_BG_INDEX;
    }
}

static int jack_assets_find_cache_slot(const char *title, int *free_slot) {
    if (free_slot) *free_slot = -1;
    if (!title || !*title) return -1;

    for (int i = 0; i < JACK_COVER_CACHE_MAX; i++) {
        if (g_cover_cache[i].state == 0) {
            if (free_slot && *free_slot < 0) *free_slot = i;
            continue;
        }
        if (kstrcmp(g_cover_cache[i].title, title) == 0) {
            return i;
        }
    }
    return -1;
}

#ifdef USE_PNG
static const uint8_t g_cover_preview_pal[16][3] = {
    {  0,   0,   0},
    {252, 252, 252},
    {226, 231, 236},
    {194, 198, 202},
    {145, 149, 153},
    { 97, 101, 105},
    {204,  48,  52},
    {228, 112,  36},
    {236, 196,  44},
    {124, 172,  60},
    { 44, 132,  72},
    { 52, 160, 192},
    { 56,  92, 188},
    {124,  92, 188},
    {164, 104,  60},
    {224, 160, 120},
};

static uint32_t jack_assets_color_distance_sq(uint8_t r0, uint8_t g0, uint8_t b0,
                                              uint8_t r1, uint8_t g1, uint8_t b1) {
    int dr = (int)r0 - (int)r1;
    int dg = (int)g0 - (int)g1;
    int db = (int)b0 - (int)b1;
    return (uint32_t)(dr * dr + dg * dg + db * db);
}

static uint8_t jack_assets_quantize_rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t best = 0;
    uint32_t best_dist = 0xFFFFFFFFu;
    for (uint8_t i = 0; i < 16; i++) {
        uint32_t dist = jack_assets_color_distance_sq(
            r, g, b,
            g_cover_preview_pal[i][0],
            g_cover_preview_pal[i][1],
            g_cover_preview_pal[i][2]
        );
        if (dist < best_dist) {
            best = i;
            best_dist = dist;
            if (dist == 0) break;
        }
    }
    return best;
}

static int jack_assets_find_opaque_bounds(const struct mImage *image,
                                          int *out_min_x, int *out_min_y,
                                          int *out_max_x, int *out_max_y) {
    int found = 0;
    int min_x = (int)image->width;
    int min_y = (int)image->height;
    int max_x = -1;
    int max_y = -1;

    for (unsigned y = 0; y < image->height; y++) {
        for (unsigned x = 0; x < image->width; x++) {
            uint32_t color = mImageGetPixel(image, x, y);
            uint8_t alpha = (uint8_t)(color >> 24);
            if (alpha < 8) continue;
            if ((int)x < min_x) min_x = (int)x;
            if ((int)y < min_y) min_y = (int)y;
            if ((int)x > max_x) max_x = (int)x;
            if ((int)y > max_y) max_y = (int)y;
            found = 1;
        }
    }

    if (!found) return 0;
    if (out_min_x) *out_min_x = min_x;
    if (out_min_y) *out_min_y = min_y;
    if (out_max_x) *out_max_x = max_x;
    if (out_max_y) *out_max_y = max_y;
    return 1;
}

static int jack_assets_decode_png_preview(const char *png_data, uint32_t png_size, uint8_t *out_pixels) {
    struct VFile *vf;
    struct mImage *image;
    int min_x;
    int min_y;
    int max_x;
    int max_y;
    int box_w;
    int box_h;
    int draw_w;
    int draw_h;
    int off_x;
    int off_y;

    if (!png_data || png_size == 0 || !out_pixels) return 0;

    vf = VFileFromConstMemory(png_data, png_size);
    if (!vf) return 0;

    image = mImageLoadVF(vf);
    vf->close(vf);
    if (!image) return 0;

    jack_assets_fill_bg(out_pixels);

    if (!jack_assets_find_opaque_bounds(image, &min_x, &min_y, &max_x, &max_y)) {
        min_x = 0;
        min_y = 0;
        max_x = (int)image->width - 1;
        max_y = (int)image->height - 1;
    }

    box_w = max_x - min_x + 1;
    box_h = max_y - min_y + 1;
    if (box_w <= 0 || box_h <= 0) {
        mImageDestroy(image);
        return 0;
    }

    if ((uint32_t)box_w * JACK_COVER_PREVIEW_H > (uint32_t)box_h * JACK_COVER_PREVIEW_W) {
        draw_w = JACK_COVER_PREVIEW_W;
        draw_h = (box_h * JACK_COVER_PREVIEW_W + box_w / 2) / box_w;
    } else {
        draw_h = JACK_COVER_PREVIEW_H;
        draw_w = (box_w * JACK_COVER_PREVIEW_H + box_h / 2) / box_h;
    }

    if (draw_w < 1) draw_w = 1;
    if (draw_h < 1) draw_h = 1;
    if (draw_w > JACK_COVER_PREVIEW_W) draw_w = JACK_COVER_PREVIEW_W;
    if (draw_h > JACK_COVER_PREVIEW_H) draw_h = JACK_COVER_PREVIEW_H;

    off_x = (JACK_COVER_PREVIEW_W - draw_w) / 2;
    off_y = (JACK_COVER_PREVIEW_H - draw_h) / 2;

    for (int dy = 0; dy < draw_h; dy++) {
        int sy = min_y + (dy * box_h + draw_h / 2) / draw_h;
        if (sy > max_y) sy = max_y;
        for (int dx = 0; dx < draw_w; dx++) {
            uint32_t color;
            uint8_t a;
            uint8_t r;
            uint8_t g;
            uint8_t b;
            int sx = min_x + (dx * box_w + draw_w / 2) / draw_w;
            if (sx > max_x) sx = max_x;

            color = mImageGetPixel(image, (unsigned)sx, (unsigned)sy);
            a = (uint8_t)(color >> 24);
            r = (uint8_t)((color >> 16) & 0xFF);
            g = (uint8_t)((color >> 8) & 0xFF);
            b = (uint8_t)(color & 0xFF);

            if (a < 255) {
                r = (uint8_t)((r * a + JACK_COVER_BG_R * (255 - a)) / 255);
                g = (uint8_t)((g * a + JACK_COVER_BG_G * (255 - a)) / 255);
                b = (uint8_t)((b * a + JACK_COVER_BG_B * (255 - a)) / 255);
            }

            out_pixels[(off_y + dy) * JACK_COVER_PREVIEW_W + (off_x + dx)] =
                jack_assets_quantize_rgb(r, g, b);
        }
    }

    mImageDestroy(image);
    return 1;
}
#endif

static int jack_assets_decode_cover_preview(const char *title, uint8_t *out_pixels) {
    char cover_name[VM_NAME_MAX];
    const char *png_data;
    uint32_t png_size = 0;

    if (!title || !*title || !out_pixels) return 0;

    ksprintf(cover_name, "%s/cover.png", title);
    png_data = vm_store_find(cover_name, &png_size);
    if (!png_data || png_size == 0) return 0;

#ifdef USE_PNG
    if (jack_assets_decode_png_preview(png_data, png_size, out_pixels)) {
        return 1;
    }
#endif

    return 0;
}

void jack_assets_mount(void) {
    jack_assets_auto_mount();

#if ALOS_HAS_GBA_BIOS
    {
        uint32_t bios_size =
            (uint32_t)(_binary_assets_gba_bios_bin_end - _binary_assets_gba_bios_bin_start);
        if (bios_size > 0) {
            vm_store_add("gba_bios.bin", (const char *)_binary_assets_gba_bios_bin_start, bios_size);
        }
    }
#endif
#if ALOS_HAS_GB_BIOS
    {
        uint32_t bios_size =
            (uint32_t)(_binary_assets_gb_bios_bin_end - _binary_assets_gb_bios_bin_start);
        if (bios_size > 0) {
            vm_store_add("gb_bios.bin", (const char *)_binary_assets_gb_bios_bin_start, bios_size);
            vm_store_add("bios/gb/gb_bios.bin", (const char *)_binary_assets_gb_bios_bin_start, bios_size);
        }
    }
#endif
#if ALOS_HAS_GB_DMG0_BIOS
    {
        uint32_t bios_size =
            (uint32_t)(_binary_assets_dmg0_rom_bin_end - _binary_assets_dmg0_rom_bin_start);
        if (bios_size > 0) {
            vm_store_add("dmg0_rom.bin", (const char *)_binary_assets_dmg0_rom_bin_start, bios_size);
            vm_store_add("bios/gb/dmg0_rom.bin", (const char *)_binary_assets_dmg0_rom_bin_start, bios_size);
        }
    }
#endif
#if ALOS_HAS_GBC_BIOS
    {
        uint32_t bios_size =
            (uint32_t)(_binary_assets_gbc_bios_bin_end - _binary_assets_gbc_bios_bin_start);
        if (bios_size > 0) {
            vm_store_add("gbc_bios.bin", (const char *)_binary_assets_gbc_bios_bin_start, bios_size);
            vm_store_add("bios/gbc/gbc_bios.bin", (const char *)_binary_assets_gbc_bios_bin_start, bios_size);
        }
    }
#endif
#if ALOS_HAS_NDS_BIOS7
    {
        uint32_t bios_size =
            (uint32_t)(_binary_assets_biosnds7_rom_end - _binary_assets_biosnds7_rom_start);
        if (bios_size > 0) {
            vm_store_add("biosnds7.rom", (const char *)_binary_assets_biosnds7_rom_start, bios_size);
            vm_store_add("bios/nds/biosnds7.rom", (const char *)_binary_assets_biosnds7_rom_start, bios_size);
        }
    }
#endif
#if ALOS_HAS_NDS_BIOS9
    {
        uint32_t bios_size =
            (uint32_t)(_binary_assets_biosnds9_rom_end - _binary_assets_biosnds9_rom_start);
        if (bios_size > 0) {
            vm_store_add("biosnds9.rom", (const char *)_binary_assets_biosnds9_rom_start, bios_size);
            vm_store_add("bios/nds/biosnds9.rom", (const char *)_binary_assets_biosnds9_rom_start, bios_size);
        }
    }
#endif
#if ALOS_HAS_NDS_FIRMWARE
    {
        uint32_t bios_size =
            (uint32_t)(_binary_assets_firmware_bin_end - _binary_assets_firmware_bin_start);
        if (bios_size > 0) {
            vm_store_add("firmware.bin", (const char *)_binary_assets_firmware_bin_start, bios_size);
            vm_store_add("bios/nds/firmware.bin", (const char *)_binary_assets_firmware_bin_start, bios_size);
        }
    }
#endif
#if ALOS_HAS_NDS_KEYCFG
    {
        uint32_t bios_size =
            (uint32_t)(_binary_assets_key_cfg_end - _binary_assets_key_cfg_start);
        if (bios_size > 0) {
            vm_store_add("key.cfg", (const char *)_binary_assets_key_cfg_start, bios_size);
            vm_store_add("bios/nds/key.cfg", (const char *)_binary_assets_key_cfg_start, bios_size);
        }
    }
#endif
}

int jack_assets_get_cover_preview(const char *title, const uint8_t **out_pixels, int *out_w, int *out_h) {
    int free_slot = -1;
    int slot = jack_assets_find_cache_slot(title, &free_slot);
    int ok = 0;

    if (slot >= 0) {
        if (g_cover_cache[slot].state != 1) return 0;
        if (out_pixels) *out_pixels = g_cover_cache[slot].pixels;
        if (out_w) *out_w = JACK_COVER_PREVIEW_W;
        if (out_h) *out_h = JACK_COVER_PREVIEW_H;
        return 1;
    }

    if (free_slot < 0) {
        free_slot = 0;
    }

    kstrncpy(g_cover_cache[free_slot].title, title ? title : "", VM_NAME_MAX - 1);
    g_cover_cache[free_slot].title[VM_NAME_MAX - 1] = '\0';
    g_cover_cache[free_slot].state = 2;
    jack_assets_fill_bg(g_cover_cache[free_slot].pixels);

    ok = jack_assets_decode_cover_preview(title, g_cover_cache[free_slot].pixels);
    if (!ok) {
        const uint8_t *fallback_pixels = 0;
        int fallback_w = 0;
        int fallback_h = 0;
        if (jack_assets_get_generated_cover_preview(title, &fallback_pixels, &fallback_w, &fallback_h) &&
            fallback_pixels &&
            fallback_w == JACK_COVER_PREVIEW_W &&
            fallback_h == JACK_COVER_PREVIEW_H) {
            kmemcpy(g_cover_cache[free_slot].pixels, fallback_pixels,
                    JACK_COVER_PREVIEW_W * JACK_COVER_PREVIEW_H);
            ok = 1;
        }
    }

    if (!ok) {
        return 0;
    }

    g_cover_cache[free_slot].state = 1;
    if (out_pixels) *out_pixels = g_cover_cache[free_slot].pixels;
    if (out_w) *out_w = JACK_COVER_PREVIEW_W;
    if (out_h) *out_h = JACK_COVER_PREVIEW_H;
    return 1;
}
