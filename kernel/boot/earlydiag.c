#include "earlydiag.h"

#include "../../driver/vga.h"
#include "../jack/font5x8.h"
#include "../lib/string.h"

#define DIAG_COLS   80
#define DIAG_ROWS   25
#define DIAG_CHAR_W 8
#define DIAG_CHAR_H 16

enum {
    DIAG_BACKEND_VGA = 0,
    DIAG_BACKEND_FB = 1,
    DIAG_BACKEND_TEXTFB = 2
};

static int g_diag_backend = DIAG_BACKEND_VGA;
static volatile uint8_t *g_diag_fb = 0;
static volatile uint16_t *g_diag_textfb = 0;
static uint32_t g_diag_textfb_pitch_words = 0;
static uint32_t g_diag_fb_pitch = 0;
static uint32_t g_diag_fb_width = 0;
static uint32_t g_diag_fb_height = 0;
static uint32_t g_diag_fb_bpp = 0;
static uint8_t g_diag_fb_red_pos = 16;
static uint8_t g_diag_fb_red_mask = 8;
static uint8_t g_diag_fb_green_pos = 8;
static uint8_t g_diag_fb_green_mask = 8;
static uint8_t g_diag_fb_blue_pos = 0;
static uint8_t g_diag_fb_blue_mask = 8;
static int g_diag_origin_x = 0;
static int g_diag_origin_y = 0;
static int g_diag_row = 0;

static const uint8_t g_diag_palette[16][3] = {
    {  0,   0,   0}, {  0,   0, 170}, {  0, 170,   0}, {  0, 170, 170},
    {170,   0,   0}, {170,   0, 170}, {170,  85,   0}, {170, 170, 170},
    { 85,  85,  85}, { 85,  85, 255}, { 85, 255,  85}, { 85, 255, 255},
    {255,  85,  85}, {255,  85, 255}, {255, 255,  85}, {255, 255, 255}
};

static uint32_t diag_rgb_from_vga(uint8_t color) {
    const uint8_t *rgb = g_diag_palette[color & 0x0F];
    return ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | rgb[2];
}

static uint32_t diag_scale_to_mask(uint32_t value, uint8_t bits) {
    uint32_t max;
    if (!bits) return 0;
    if (bits >= 8) {
        if (bits >= 31) return value;
        return value << (bits - 8);
    }
    max = (1u << bits) - 1u;
    return (value * max + 127u) / 255u;
}

static uint32_t diag_pack_rgb(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFFu;
    uint32_t g = (rgb >> 8) & 0xFFu;
    uint32_t b = rgb & 0xFFu;
    uint32_t packed = 0;
    packed |= diag_scale_to_mask(r, g_diag_fb_red_mask) << g_diag_fb_red_pos;
    packed |= diag_scale_to_mask(g, g_diag_fb_green_mask) << g_diag_fb_green_pos;
    packed |= diag_scale_to_mask(b, g_diag_fb_blue_mask) << g_diag_fb_blue_pos;
    return packed;
}

static void diag_fb_put_pixel(int x, int y, uint32_t rgb) {
    volatile uint8_t *dst;
    uint32_t packed;
    if (!g_diag_fb) return;
    if ((unsigned)x >= g_diag_fb_width || (unsigned)y >= g_diag_fb_height) return;
    dst = g_diag_fb + y * g_diag_fb_pitch + x * (g_diag_fb_bpp / 8u);
    packed = diag_pack_rgb(rgb);
    if (g_diag_fb_bpp == 32) {
        dst[0] = (uint8_t)(packed & 0xFFu);
        dst[1] = (uint8_t)((packed >> 8) & 0xFFu);
        dst[2] = (uint8_t)((packed >> 16) & 0xFFu);
        dst[3] = (uint8_t)((packed >> 24) & 0xFFu);
    } else if (g_diag_fb_bpp == 24) {
        dst[0] = (uint8_t)(packed & 0xFFu);
        dst[1] = (uint8_t)((packed >> 8) & 0xFFu);
        dst[2] = (uint8_t)((packed >> 16) & 0xFFu);
    } else if (g_diag_fb_bpp == 16) {
        dst[0] = (uint8_t)(packed & 0xFFu);
        dst[1] = (uint8_t)((packed >> 8) & 0xFFu);
    }
}

static void diag_fb_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            diag_fb_put_pixel(x + xx, y + yy, rgb);
        }
    }
}

static void diag_fb_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg) {
    int px = g_diag_origin_x + x * DIAG_CHAR_W;
    int py = g_diag_origin_y + y * DIAG_CHAR_H;
    uint32_t bg_rgb = diag_rgb_from_vga(bg);
    uint32_t fg_rgb = diag_rgb_from_vga(fg);

    diag_fb_fill_rect(px, py, DIAG_CHAR_W, DIAG_CHAR_H, bg_rgb);
    for (int cy = 0; cy < DIAG_CHAR_H; ++cy) {
        int gy = cy >> 1;
        for (int cx = 0; cx < DIAG_CHAR_W; ++cx) {
            int gx = (cx * 5) >> 3;
            if ((hack_font5x8[(uint8_t)c][gx] >> gy) & 1) {
                diag_fb_put_pixel(px + cx, py + cy, fg_rgb);
            }
        }
    }
}

static void diag_textfb_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg) {
    volatile uint16_t *cell;
    uint8_t attr = VGA_COLOR(bg, fg);
    if (!g_diag_textfb) return;
    if ((unsigned)x >= DIAG_COLS || (unsigned)y >= DIAG_ROWS) return;
    cell = g_diag_textfb + y * g_diag_textfb_pitch_words + x;
    *cell = (uint16_t)(((uint16_t)attr << 8) | (uint8_t)c);
}

static void diag_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg) {
    if (g_diag_backend == DIAG_BACKEND_FB) {
        diag_fb_draw_char(x, y, c, fg, bg);
    } else if (g_diag_backend == DIAG_BACKEND_TEXTFB) {
        diag_textfb_draw_char(x, y, c, fg, bg);
    } else {
        vga_put_char(x, y, c, VGA_COLOR(bg, fg));
    }
}

static void diag_draw_text(int x, int y, const char *text, uint8_t fg, uint8_t bg) {
    int cx = x;
    while (text && *text && cx < DIAG_COLS) {
        diag_draw_char(cx++, y, *text++, fg, bg);
    }
}

static void diag_clear(uint8_t bg) {
    if (g_diag_backend == DIAG_BACKEND_FB) {
        diag_fb_fill_rect(0, 0, (int)g_diag_fb_width, (int)g_diag_fb_height, diag_rgb_from_vga(bg));
    } else if (g_diag_backend == DIAG_BACKEND_TEXTFB) {
        for (int y = 0; y < DIAG_ROWS; ++y) {
            for (int x = 0; x < DIAG_COLS; ++x) {
                diag_textfb_draw_char(x, y, ' ', VGA_LIGHT_GREY, bg);
            }
        }
    } else {
        vga_clear();
    }
}

static void diag_write_banner(const char *text, uint8_t fg, uint8_t bg) {
    diag_draw_text(1, 1, text, fg, bg);
}

void earlydiag_init(const BootInfo *bi) {
    uint8_t fb_addr_usable = 0;

    g_diag_backend = DIAG_BACKEND_VGA;
    g_diag_fb = 0;
    g_diag_textfb = 0;
    g_diag_textfb_pitch_words = 0;
    g_diag_fb_pitch = 0;
    g_diag_fb_width = 0;
    g_diag_fb_height = 0;
    g_diag_fb_bpp = 0;
    g_diag_fb_red_pos = 16;
    g_diag_fb_red_mask = 8;
    g_diag_fb_green_pos = 8;
    g_diag_fb_green_mask = 8;
    g_diag_fb_blue_pos = 0;
    g_diag_fb_blue_mask = 8;
    g_diag_origin_x = 0;
    g_diag_origin_y = 0;
    g_diag_row = 3;

    if (bi && bi->fb_addr && (bi->fb_addr >> 32) == 0) {
        fb_addr_usable = 1;
    }

    if (bi &&
        bi->has_framebuffer &&
        fb_addr_usable &&
        bi->fb_type == 2 &&
        bi->fb_bpp == 16 &&
        bi->fb_width >= DIAG_COLS &&
        bi->fb_height >= DIAG_ROWS &&
        bi->fb_pitch >= (DIAG_COLS * 2u)) {
        g_diag_backend = DIAG_BACKEND_TEXTFB;
        g_diag_textfb = (volatile uint16_t *)(uintptr_t)(uint32_t)bi->fb_addr;
        g_diag_textfb_pitch_words = bi->fb_pitch / 2u;
    } else if (bi &&
               bi->has_framebuffer &&
               fb_addr_usable &&
               bi->fb_type == 1 &&
               (bi->fb_bpp == 16 || bi->fb_bpp == 24 || bi->fb_bpp == 32) &&
               bi->fb_width >= (DIAG_COLS * DIAG_CHAR_W) &&
               bi->fb_height >= (DIAG_ROWS * DIAG_CHAR_H)) {
        g_diag_backend = DIAG_BACKEND_FB;
        g_diag_fb = (volatile uint8_t *)(uintptr_t)(uint32_t)bi->fb_addr;
        g_diag_fb_pitch = bi->fb_pitch;
        g_diag_fb_width = bi->fb_width;
        g_diag_fb_height = bi->fb_height;
        g_diag_fb_bpp = bi->fb_bpp;
        g_diag_fb_red_pos = bi->fb_red_pos;
        g_diag_fb_red_mask = bi->fb_red_mask;
        g_diag_fb_green_pos = bi->fb_green_pos;
        g_diag_fb_green_mask = bi->fb_green_mask;
        g_diag_fb_blue_pos = bi->fb_blue_pos;
        g_diag_fb_blue_mask = bi->fb_blue_mask;
        g_diag_origin_x = (int)(g_diag_fb_width - (DIAG_COLS * DIAG_CHAR_W)) / 2;
        g_diag_origin_y = (int)(g_diag_fb_height - (DIAG_ROWS * DIAG_CHAR_H)) / 2;
        if (g_diag_origin_x < 0) g_diag_origin_x = 0;
        if (g_diag_origin_y < 0) g_diag_origin_y = 0;
    } else {
        vga_init();
    }

    diag_clear(VGA_BLACK);
    diag_write_banner("ALOS EARLY BOOT", VGA_WHITE, VGA_BLACK);
}

void earlydiag_stage(const char *msg) {
    int row = g_diag_row;
    if (row < 3) row = 3;
    if (row >= (DIAG_ROWS - 1)) row = DIAG_ROWS - 2;
    diag_draw_text(1, row, "                                                                                ", VGA_LIGHT_GREY, VGA_BLACK);
    diag_draw_text(1, row, msg ? msg : "(null)", VGA_LIGHT_CYAN, VGA_BLACK);
    if (g_diag_row < (DIAG_ROWS - 2)) {
        ++g_diag_row;
    }
}

void earlydiag_panic_num(const char *title, uint32_t code) {
    char num[11];
    int n = 0;

    diag_clear(VGA_RED);
    diag_write_banner("ALOS PANIC", VGA_WHITE, VGA_RED);
    diag_draw_text(1, 4, title ? title : "panic", VGA_WHITE, VGA_RED);

    if (code == 0) {
        num[n++] = '0';
    } else {
        char rev[11];
        int r = 0;
        while (code && r < (int)sizeof(rev)) {
            rev[r++] = (char)('0' + (code % 10u));
            code /= 10u;
        }
        while (r > 0) {
            num[n++] = rev[--r];
        }
    }
    num[n] = '\0';
    diag_draw_text(1, 6, "code:", VGA_WHITE, VGA_RED);
    diag_draw_text(7, 6, num, VGA_YELLOW, VGA_RED);
    diag_draw_text(1, 8, "Le noyau s'est arrete sur une exception CPU.", VGA_WHITE, VGA_RED);
}
