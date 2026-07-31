/**
 * kernel/tty.c - terminal texte ALOS avec backend VGA ou framebuffer UEFI/GRUB
 */

#include "tty.h"
#include "../driver/vga.h"
#include "boot/bootinfo.h"
#include "ui/font5x8.h"
#include "lib/string.h"

#define SCROLL_TOP    0
#define SCROLL_BOT   22
#define SEP_ROW      23
#define INPUT_ROW    24

#define PROMPT       "root@alos:~$ "
#define PROMPT_LEN   13

#define C_DEFAULT  VGA_COLOR(VGA_BLACK, VGA_LIGHT_GREY)
#define C_OUTPUT   VGA_COLOR(VGA_BLACK, VGA_WHITE)
#define C_CMD      VGA_COLOR(VGA_BLACK, VGA_LIGHT_GREEN)
#define C_PROMPT   VGA_COLOR(VGA_BLACK, VGA_GREEN)
#define C_SEP      VGA_COLOR(VGA_BLACK, VGA_DARK_GREY)
#define C_INPUT    VGA_COLOR(VGA_BLACK, VGA_WHITE)

#define TTY_COLS 80
#define TTY_ROWS 25
#define FB_CHAR_W 8
#define FB_CHAR_H 16

static int cur_row = SCROLL_TOP;
static int cur_col = 0;

static uint8_t g_chars[TTY_COLS * TTY_ROWS];
static uint8_t g_attrs[TTY_COLS * TTY_ROWS];

static int g_use_fb = 0;
static int g_use_textfb = 0;
static volatile uint8_t *g_fb = 0;
static volatile uint16_t *g_textfb = 0;
static uint32_t g_textfb_pitch_words = 0;
static uint32_t g_fb_pitch = 0;
static uint32_t g_fb_width = 0;
static uint32_t g_fb_height = 0;
static uint32_t g_fb_bpp = 0;
static uint8_t g_fb_red_pos = 16;
static uint8_t g_fb_red_mask = 8;
static uint8_t g_fb_green_pos = 8;
static uint8_t g_fb_green_mask = 8;
static uint8_t g_fb_blue_pos = 0;
static uint8_t g_fb_blue_mask = 8;
static int g_fb_origin_x = 0;
static int g_fb_origin_y = 0;

static const uint8_t g_vga_palette_rgb[16][3] = {
    {  0,   0,   0}, {  0,   0, 170}, {  0, 170,   0}, {  0, 170, 170},
    {170,   0,   0}, {170,   0, 170}, {170,  85,   0}, {170, 170, 170},
    { 85,  85,  85}, { 85,  85, 255}, { 85, 255,  85}, { 85, 255, 255},
    {255,  85,  85}, {255,  85, 255}, {255, 255,  85}, {255, 255, 255}
};

static inline int tty_idx(int x, int y) {
    return y * TTY_COLS + x;
}

static uint32_t tty_rgb(uint8_t vga_color) {
    const uint8_t *rgb = g_vga_palette_rgb[vga_color & 0x0F];
    return ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | rgb[2];
}

static uint32_t fb_scale_to_mask(uint32_t value, uint8_t bits) {
    uint32_t max;
    if (!bits) return 0;
    if (bits >= 8) {
        if (bits >= 31) return value;
        return value << (bits - 8);
    }
    max = (1u << bits) - 1u;
    return (value * max + 127u) / 255u;
}

static uint32_t fb_pack_rgb(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFFu;
    uint32_t g = (rgb >> 8) & 0xFFu;
    uint32_t b = rgb & 0xFFu;
    uint32_t packed = 0;
    packed |= fb_scale_to_mask(r, g_fb_red_mask) << g_fb_red_pos;
    packed |= fb_scale_to_mask(g, g_fb_green_mask) << g_fb_green_pos;
    packed |= fb_scale_to_mask(b, g_fb_blue_mask) << g_fb_blue_pos;
    return packed;
}

static void fb_put_pixel(int x, int y, uint32_t rgb) {
    volatile uint8_t *dst;
    uint32_t packed;
    if (!g_fb) return;
    if ((unsigned)x >= g_fb_width || (unsigned)y >= g_fb_height) return;
    dst = g_fb + y * g_fb_pitch + x * (g_fb_bpp / 8u);
    packed = fb_pack_rgb(rgb);
    if (g_fb_bpp == 32) {
        dst[0] = (uint8_t)(packed & 0xFFu);
        dst[1] = (uint8_t)((packed >> 8) & 0xFFu);
        dst[2] = (uint8_t)((packed >> 16) & 0xFFu);
        dst[3] = (uint8_t)((packed >> 24) & 0xFFu);
    } else if (g_fb_bpp == 24) {
        dst[0] = (uint8_t)(packed & 0xFFu);
        dst[1] = (uint8_t)((packed >> 8) & 0xFFu);
        dst[2] = (uint8_t)((packed >> 16) & 0xFFu);
    } else if (g_fb_bpp == 16) {
        dst[0] = (uint8_t)(packed & 0xFFu);
        dst[1] = (uint8_t)((packed >> 8) & 0xFFu);
    }
}

static void fb_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            fb_put_pixel(x + xx, y + yy, rgb);
        }
    }
}

static void fb_draw_cell(int x, int y, char c, uint8_t attr) {
    int px = g_fb_origin_x + x * FB_CHAR_W;
    int py = g_fb_origin_y + y * FB_CHAR_H;
    uint32_t bg = tty_rgb((attr >> 4) & 0x0F);
    uint32_t fg = tty_rgb(attr & 0x0F);

    fb_fill_rect(px, py, FB_CHAR_W, FB_CHAR_H, bg);
    for (int cy = 0; cy < FB_CHAR_H; cy++) {
        int gy = cy >> 1;
        for (int cx = 0; cx < FB_CHAR_W; cx++) {
            int gx = (cx * 5) >> 3;
            if ((hack_font5x8[(uint8_t)c][gx] >> gy) & 1) {
                fb_put_pixel(px + cx, py + cy, fg);
            }
        }
    }
}

static void textfb_draw_cell(int x, int y, char c, uint8_t attr) {
    volatile uint16_t *cell;
    if (!g_textfb) return;
    if ((unsigned)x >= TTY_COLS || (unsigned)y >= TTY_ROWS) return;
    cell = g_textfb + y * g_textfb_pitch_words + x;
    *cell = (uint16_t)(((uint16_t)attr << 8) | (uint8_t)c);
}

static void tty_draw_cell(int x, int y) {
    int idx = tty_idx(x, y);
    if (g_use_fb) {
        fb_draw_cell(x, y, (char)g_chars[idx], g_attrs[idx]);
    } else if (g_use_textfb) {
        textfb_draw_cell(x, y, (char)g_chars[idx], g_attrs[idx]);
    } else {
        vga_put_char(x, y, (char)g_chars[idx], g_attrs[idx]);
    }
}

static void tty_redraw_all(void) {
    if (g_use_fb) {
        fb_fill_rect(0, 0, (int)g_fb_width, (int)g_fb_height, tty_rgb(VGA_BLACK));
    }
    for (int y = 0; y < TTY_ROWS; y++) {
        for (int x = 0; x < TTY_COLS; x++) {
            tty_draw_cell(x, y);
        }
    }
}

static void tty_fill_row(int row, char c, uint8_t attr) {
    for (int x = 0; x < TTY_COLS; x++) {
        int idx = tty_idx(x, row);
        g_chars[idx] = (uint8_t)c;
        g_attrs[idx] = attr;
    }
}

static void draw_separator(void) {
    tty_fill_row(SEP_ROW, '-', C_SEP);
    for (int x = 0; x < TTY_COLS; x++) tty_draw_cell(x, SEP_ROW);
}

static void tty_draw_prompt(void) {
    for (int x = 0; x < TTY_COLS; x++) {
        int idx = tty_idx(x, INPUT_ROW);
        g_chars[idx] = ' ';
        g_attrs[idx] = C_DEFAULT;
    }
    for (int i = 0; PROMPT[i]; i++) {
        int idx = tty_idx(i, INPUT_ROW);
        g_chars[idx] = (uint8_t)PROMPT[i];
        g_attrs[idx] = C_PROMPT;
    }
    for (int x = 0; x < TTY_COLS; x++) tty_draw_cell(x, INPUT_ROW);
}

static void scroll_up(void) {
    for (int y = SCROLL_TOP; y < SCROLL_BOT; y++) {
        for (int x = 0; x < TTY_COLS; x++) {
            int dst = tty_idx(x, y);
            int src = tty_idx(x, y + 1);
            g_chars[dst] = g_chars[src];
            g_attrs[dst] = g_attrs[src];
        }
    }
    tty_fill_row(SCROLL_BOT, ' ', C_DEFAULT);
    if (g_use_fb) {
        tty_redraw_all();
    } else {
        for (int y = SCROLL_TOP; y <= SCROLL_BOT; y++) {
            for (int x = 0; x < TTY_COLS; x++) tty_draw_cell(x, y);
        }
    }
}

static void newline(void) {
    cur_col = 0;
    if (cur_row < SCROLL_BOT) cur_row++;
    else scroll_up();
}

int tty_is_framebuffer(void) {
    return g_use_fb || g_use_textfb;
}

void tty_init(void) {
    const BootInfo *bi = bootinfo_get();
    uint8_t fb_addr_usable = 0;
    g_use_fb = 0;
    g_use_textfb = 0;
    g_fb = 0;
    g_textfb = 0;
    g_textfb_pitch_words = 0;
    g_fb_pitch = 0;
    g_fb_width = 0;
    g_fb_height = 0;
    g_fb_bpp = 0;
    g_fb_red_pos = 16;
    g_fb_red_mask = 8;
    g_fb_green_pos = 8;
    g_fb_green_mask = 8;
    g_fb_blue_pos = 0;
    g_fb_blue_mask = 8;
    g_fb_origin_x = 0;
    g_fb_origin_y = 0;

    if (bi && bi->fb_addr && (bi->fb_addr >> 32) == 0) {
        fb_addr_usable = 1;
    }

    if (bi &&
        bi->has_framebuffer &&
        fb_addr_usable &&
        bi->fb_type == 2 &&
        bi->fb_bpp == 16 &&
        bi->fb_width >= TTY_COLS &&
        bi->fb_height >= TTY_ROWS &&
        bi->fb_pitch >= (TTY_COLS * 2u)) {
        g_use_textfb = 1;
        g_textfb = (volatile uint16_t *)(uintptr_t)(uint32_t)bi->fb_addr;
        g_textfb_pitch_words = bi->fb_pitch / 2u;
    } else if (bi &&
        bi->has_framebuffer &&
        fb_addr_usable &&
        bi->fb_type == 1 &&
        (bi->fb_bpp == 16 || bi->fb_bpp == 24 || bi->fb_bpp == 32) &&
        bi->fb_width >= (TTY_COLS * FB_CHAR_W) &&
        bi->fb_height >= (TTY_ROWS * FB_CHAR_H)) {
        g_use_fb = 1;
        g_fb = (volatile uint8_t *)(uintptr_t)(uint32_t)bi->fb_addr;
        g_fb_pitch = bi->fb_pitch;
        g_fb_width = bi->fb_width;
        g_fb_height = bi->fb_height;
        g_fb_bpp = bi->fb_bpp;
        g_fb_red_pos = bi->fb_red_pos;
        g_fb_red_mask = bi->fb_red_mask;
        g_fb_green_pos = bi->fb_green_pos;
        g_fb_green_mask = bi->fb_green_mask;
        g_fb_blue_pos = bi->fb_blue_pos;
        g_fb_blue_mask = bi->fb_blue_mask;
        g_fb_origin_x = (int)(g_fb_width - (TTY_COLS * FB_CHAR_W)) / 2;
        g_fb_origin_y = (int)(g_fb_height - (TTY_ROWS * FB_CHAR_H)) / 2;
        if (g_fb_origin_x < 0) g_fb_origin_x = 0;
        if (g_fb_origin_y < 0) g_fb_origin_y = 0;
    } else {
        vga_init();
    }

    for (int y = 0; y < TTY_ROWS; y++) {
        tty_fill_row(y, ' ', C_DEFAULT);
    }
    draw_separator();
    tty_draw_prompt();
    cur_row = SCROLL_TOP;
    cur_col = 0;
    tty_redraw_all();
}

void tty_write_color(const char *str, uint8_t color) {
    while (*str) {
        char c = *str++;
        if (c == '\n') {
            newline();
            continue;
        }
        if (c == '\r') {
            cur_col = 0;
            continue;
        }
        if (cur_col >= TTY_COLS) newline();
        {
            int idx = tty_idx(cur_col, cur_row);
            g_chars[idx] = (uint8_t)c;
            g_attrs[idx] = color;
            tty_draw_cell(cur_col, cur_row);
        }
        cur_col++;
    }
    newline();
}

void tty_write(const char *str) {
    tty_write_color(str, C_OUTPUT);
}

void tty_echo_cmd(const char *str) {
    const char *prompt = PROMPT;
    while (*prompt) {
        if (cur_col >= TTY_COLS) newline();
        {
            int idx = tty_idx(cur_col, cur_row);
            g_chars[idx] = (uint8_t)(*prompt++);
            g_attrs[idx] = C_PROMPT;
            tty_draw_cell(cur_col, cur_row);
            cur_col++;
        }
    }
    while (*str) {
        if (cur_col >= TTY_COLS) newline();
        {
            int idx = tty_idx(cur_col, cur_row);
            g_chars[idx] = (uint8_t)(*str++);
            g_attrs[idx] = C_CMD;
            tty_draw_cell(cur_col, cur_row);
            cur_col++;
        }
    }
    newline();
}

void tty_clear_output(void) {
    for (int r = SCROLL_TOP; r <= SCROLL_BOT; r++) {
        tty_fill_row(r, ' ', C_DEFAULT);
    }
    cur_row = SCROLL_TOP;
    cur_col = 0;
    if (g_use_fb) tty_redraw_all();
    else {
        for (int y = SCROLL_TOP; y <= SCROLL_BOT; y++) {
            for (int x = 0; x < TTY_COLS; x++) tty_draw_cell(x, y);
        }
    }
}

void tty_clear_input(void) {
    tty_draw_prompt();
}

void tty_echo_char(char c, int cx) {
    int x = PROMPT_LEN + cx;
    if ((unsigned)x >= TTY_COLS) return;
    g_chars[tty_idx(x, INPUT_ROW)] = (uint8_t)c;
    g_attrs[tty_idx(x, INPUT_ROW)] = C_INPUT;
    tty_draw_cell(x, INPUT_ROW);
}

void tty_backspace(int cx) {
    int x = PROMPT_LEN + cx;
    if ((unsigned)x >= TTY_COLS) return;
    g_chars[tty_idx(x, INPUT_ROW)] = ' ';
    g_attrs[tty_idx(x, INPUT_ROW)] = C_DEFAULT;
    tty_draw_cell(x, INPUT_ROW);
}

int tty_prompt_len(void) {
    return PROMPT_LEN;
}
