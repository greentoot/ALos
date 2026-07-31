#include "gfx.h"
#include "../kernel/ui/font5x8.h"

/* VGA ports */
#define VGA_MISC_WRITE   0x3C2
#define VGA_MISC_READ    0x3CC
#define VGA_SEQ_INDEX    0x3C4
#define VGA_SEQ_DATA     0x3C5
#define VGA_CRTC_INDEX   0x3D4
#define VGA_CRTC_DATA    0x3D5
#define VGA_GC_INDEX     0x3CE
#define VGA_GC_DATA      0x3CF
#define VGA_AC_INDEX     0x3C0
#define VGA_AC_READ      0x3C1
#define VGA_INSTAT_READ  0x3DA
#define VGA_DAC_MASK     0x3C6
#define VGA_DAC_WRITE    0x3C8
#define VGA_DAC_DATA     0x3C9

/* Bochs/QEMU VBE DISPI ports */
#define VBE_DISPI_IOPORT_INDEX   0x01CE
#define VBE_DISPI_IOPORT_DATA    0x01CF
#define VBE_DISPI_INDEX_ID       0x0
#define VBE_DISPI_INDEX_XRES     0x1
#define VBE_DISPI_INDEX_YRES     0x2
#define VBE_DISPI_INDEX_BPP      0x3
#define VBE_DISPI_INDEX_ENABLE   0x4
#define VBE_DISPI_INDEX_BANK     0x5
#define VBE_DISPI_INDEX_VIRT_W   0x6

#define VBE_DISPI_DISABLED       0x00
#define VBE_DISPI_ENABLED        0x01
#define VBE_DISPI_LFB_ENABLED    0x40

#define VBE_X2_W (HACK_W * 2)
#define VBE_X2_H (HACK_H * 2)
#define VBE_X3_W (HACK_W * 3)
#define VBE_X3_H (HACK_H * 3)

#define GFX_W 320
#define GFX_H 200
#define HACK_W 512
#define HACK_H 256
#define HACK_WORDS_PER_ROW 32

static volatile uint8_t *const g_fb13  = (volatile uint8_t *)0xA0000;

static uint8_t g_saved_misc;
static uint8_t g_saved_seq[5];
static uint8_t g_saved_crtc[25];
static uint8_t g_saved_gc[9];
static uint8_t g_saved_ac[21];
static int     g_saved_valid = 0;
static int     g_graphics    = 0;
static int     g_backend     = 0; /* 0=text, 1=vbe banked scaled, 2=vga13 */
static int     g_pref_w      = 0;
static int     g_pref_h      = 0;
static int     g_vbe_w       = VBE_X3_W;
static int     g_vbe_pitch   = VBE_X3_W;
static int     g_vbe_h       = VBE_X3_H;
static int     g_hack_scale  = 3;
static int     g_view_x      = 0;
static int     g_view_y      = 0;
static int     g_view_w      = VBE_X3_W;
static int     g_view_h      = VBE_X3_H;
static uint8_t g_expand8x1[256][8];
static uint8_t g_expand8x2[256][16];
static uint8_t g_expand8x3[256][24];
static uint8_t g_scale_row[HACK_W * 3];
static int     g_expand8_init = 0;
static uint16_t g_shadow_words[HACK_H * HACK_WORDS_PER_ROW];
static int      g_shadow_valid = 0;
static uint8_t  g_saved_font[8192];
static int      g_saved_font_valid = 0;

static void vga_load_text_font_from_5x8(void);

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile ("inw %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0,%1" :: "a"(v), "Nd"(port));
}

static inline void vbe_write(uint16_t idx, uint16_t val) {
    outw(VBE_DISPI_IOPORT_INDEX, idx);
    outw(VBE_DISPI_IOPORT_DATA, val);
}

static inline uint16_t vbe_read(uint16_t idx) {
    outw(VBE_DISPI_IOPORT_INDEX, idx);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static inline void vbe_set_bank(uint16_t bank64k) {
    vbe_write(VBE_DISPI_INDEX_BANK, bank64k);
}

static void gfx_init_expand8(void) {
    if (g_expand8_init) return;
    for (int v = 0; v < 256; v++) {
        for (int b = 0; b < 8; b++) {
            int bit = (v >> b) & 1;   /* Hack: 1=black */
            uint8_t pix = (uint8_t)(bit ? 0 : 1);
            g_expand8x1[v][b]         = pix;
            g_expand8x2[v][b * 2]     = pix;
            g_expand8x2[v][b * 2 + 1] = pix;
            g_expand8x3[v][b * 3]     = pix;
            g_expand8x3[v][b * 3 + 1] = pix;
            g_expand8x3[v][b * 3 + 2] = pix;
        }
    }
    g_expand8_init = 1;
}

static void vga_set_regs(uint8_t misc,
                         const uint8_t *seq,
                         const uint8_t *crtc,
                         const uint8_t *gc,
                         const uint8_t *ac) {
    uint8_t crtc_work[25];
    for (int i = 0; i < 25; i++) crtc_work[i] = crtc[i];

    /* CRTC unlock discipline from common VGA init sequences. */
    crtc_work[0x03] |= 0x80;
    crtc_work[0x11] &= 0x7F;

    outb(VGA_MISC_WRITE, misc);

    for (uint8_t i = 0; i < 5; i++) {
        outb(VGA_SEQ_INDEX, i);
        outb(VGA_SEQ_DATA, seq[i]);
    }

    /* Deverrouille CRTC 0x03 et 0x11 avant ecriture */
    outb(VGA_CRTC_INDEX, 0x03);
    outb(VGA_CRTC_DATA, (uint8_t)(inb(VGA_CRTC_DATA) | 0x80));
    outb(VGA_CRTC_INDEX, 0x11);
    outb(VGA_CRTC_DATA, (uint8_t)(inb(VGA_CRTC_DATA) & 0x7F));

    for (uint8_t i = 0; i < 25; i++) {
        outb(VGA_CRTC_INDEX, i);
        outb(VGA_CRTC_DATA, crtc_work[i]);
    }

    for (uint8_t i = 0; i < 9; i++) {
        outb(VGA_GC_INDEX, i);
        outb(VGA_GC_DATA, gc[i]);
    }

    for (uint8_t i = 0; i < 21; i++) {
        (void)inb(VGA_INSTAT_READ);   /* reset flip-flop */
        outb(VGA_AC_INDEX, i);
        outb(VGA_AC_INDEX, ac[i]);
    }

    /* Re-enable display output */
    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, 0x20);
}

static void vga_dac_write(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    outb(VGA_DAC_WRITE, idx);
    outb(VGA_DAC_DATA, r);
    outb(VGA_DAC_DATA, g);
    outb(VGA_DAC_DATA, b);
}

static inline void vga_dac_unmask(void) {
    outb(VGA_DAC_MASK, 0xFF);
}

static void gfx_wait_vblank(void) {
    /* Standard VGA retrace sync: wait end of current retrace, then next retrace. */
    while (inb(VGA_INSTAT_READ) & 0x08) { }
    while (!(inb(VGA_INSTAT_READ) & 0x08)) { }
}

static void vbe_mem_write(uint32_t off, const uint8_t *src, int len, uint16_t *cur_bank) {
    while (len > 0) {
        uint16_t bank = (uint16_t)(off >> 16);
        int in_bank = (int)(off & 0xFFFFu);
        int chunk = 65536 - in_bank;
        if (chunk > len) chunk = len;

        if (bank != *cur_bank) {
            vbe_set_bank(bank);
            *cur_bank = bank;
        }

        volatile uint8_t *dst = g_fb13 + in_bank;
        for (int i = 0; i < chunk; i++) dst[i] = src[i];

        off += (uint32_t)chunk;
        src += chunk;
        len -= chunk;
    }
}

static void vga_load_text_palette_16(void) {
    static const uint8_t p[16][3] = {
        {0x00,0x00,0x00}, {0x00,0x00,0x2A}, {0x00,0x2A,0x00}, {0x00,0x2A,0x2A},
        {0x2A,0x00,0x00}, {0x2A,0x00,0x2A}, {0x2A,0x15,0x00}, {0x2A,0x2A,0x2A},
        {0x15,0x15,0x15}, {0x15,0x15,0x3F}, {0x15,0x3F,0x15}, {0x15,0x3F,0x3F},
        {0x3F,0x15,0x15}, {0x3F,0x15,0x3F}, {0x3F,0x3F,0x15}, {0x3F,0x3F,0x3F}
    };
    for (uint8_t i = 0; i < 16; i++) {
        vga_dac_write(i, p[i][0], p[i][1], p[i][2]);
    }
}

static void vga_font_plane_enter(uint8_t *seq2, uint8_t *seq4,
                                 uint8_t *gc4, uint8_t *gc5, uint8_t *gc6) {
    outb(VGA_SEQ_INDEX, 0x02); *seq2 = inb(VGA_SEQ_DATA);
    outb(VGA_SEQ_INDEX, 0x04); *seq4 = inb(VGA_SEQ_DATA);
    outb(VGA_GC_INDEX,  0x04); *gc4  = inb(VGA_GC_DATA);
    outb(VGA_GC_INDEX,  0x05); *gc5  = inb(VGA_GC_DATA);
    outb(VGA_GC_INDEX,  0x06); *gc6  = inb(VGA_GC_DATA);

    /* Select plane 2 (font) at A0000. */
    outb(VGA_SEQ_INDEX, 0x02); outb(VGA_SEQ_DATA, 0x04); /* write plane 2 */
    outb(VGA_SEQ_INDEX, 0x04); outb(VGA_SEQ_DATA, 0x07); /* disable odd/even */
    outb(VGA_GC_INDEX,  0x04); outb(VGA_GC_DATA,  0x02); /* read map 2 */
    outb(VGA_GC_INDEX,  0x05); outb(VGA_GC_DATA,  0x00); /* write mode 0 */
    outb(VGA_GC_INDEX,  0x06); outb(VGA_GC_DATA,  0x00); /* A000 */
}

static void vga_font_plane_leave(uint8_t seq2, uint8_t seq4,
                                 uint8_t gc4, uint8_t gc5, uint8_t gc6) {
    outb(VGA_SEQ_INDEX, 0x02); outb(VGA_SEQ_DATA, seq2);
    outb(VGA_SEQ_INDEX, 0x04); outb(VGA_SEQ_DATA, seq4);
    outb(VGA_GC_INDEX,  0x04); outb(VGA_GC_DATA,  gc4);
    outb(VGA_GC_INDEX,  0x05); outb(VGA_GC_DATA,  gc5);
    outb(VGA_GC_INDEX,  0x06); outb(VGA_GC_DATA,  gc6);
}

static void vga_capture_text_font(void) {
    uint8_t seq2, seq4, gc4, gc5, gc6;
    volatile uint8_t *font_mem = (volatile uint8_t *)0xA0000;
    vga_font_plane_enter(&seq2, &seq4, &gc4, &gc5, &gc6);
    for (int i = 0; i < 8192; i++) g_saved_font[i] = font_mem[i];
    vga_font_plane_leave(seq2, seq4, gc4, gc5, gc6);
    g_saved_font_valid = 1;
}

static void vga_restore_text_font(void) {
    uint8_t seq2, seq4, gc4, gc5, gc6;
    volatile uint8_t *font_mem = (volatile uint8_t *)0xA0000;

    if (!g_saved_font_valid) {
        vga_load_text_font_from_5x8();
        return;
    }

    vga_font_plane_enter(&seq2, &seq4, &gc4, &gc5, &gc6);
    for (int i = 0; i < 8192; i++) font_mem[i] = g_saved_font[i];
    vga_font_plane_leave(seq2, seq4, gc4, gc5, gc6);
}

static void vga_load_text_font_from_5x8(void) {
    volatile uint8_t *font_mem = (volatile uint8_t *)0xA0000;
    uint8_t seq2, seq4, gc4, gc5, gc6;
    vga_font_plane_enter(&seq2, &seq4, &gc4, &gc5, &gc6);

    for (int ch = 0; ch < 256; ch++) {
        for (int row = 0; row < 16; row++) {
            int gy = row >> 1; /* vertical x2: 8 -> 16 */
            uint8_t r = 0;
            /* Horizontal stretch 5 -> 8 with nearest-neighbor mapping. */
            for (int x = 0; x < 8; x++) {
                int gx = (x * 5) >> 3; /* 0..4 */
                if ((hack_font5x8[ch][gx] >> gy) & 1) {
                    r |= (uint8_t)(1u << (7 - x));
                }
            }
            font_mem[ch * 32 + row] = r;
        }
        for (int row = 16; row < 32; row++) {
            font_mem[ch * 32 + row] = 0;
        }
    }

    vga_font_plane_leave(seq2, seq4, gc4, gc5, gc6);
}

/* Register set for VGA 320x200x256 (mode 13h equivalent) */
static const uint8_t g_mode13_misc = 0x63;
static const uint8_t g_mode13_seq[5] = {
    0x03, 0x01, 0x0F, 0x00, 0x0E
};
static const uint8_t g_mode13_crtc[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9C,
    0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF,
    0x00
};
static const uint8_t g_mode13_gc[9] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF
};
static const uint8_t g_mode13_ac[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};

/* Register set for VGA 80x25 text (mode 03h-like). */
static const uint8_t g_mode3_misc = 0x67;
static const uint8_t g_mode3_seq[5] = {
    0x03, 0x00, 0x03, 0x00, 0x02
};
static const uint8_t g_mode3_crtc[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
    0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF
};
static const uint8_t g_mode3_gc[9] = {
    0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF
};
static const uint8_t g_mode3_ac[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x0C, 0x00, 0x0F, 0x08, 0x00
};

static int clamp_hack_scale(int w, int h) {
    int sx = w / HACK_W;
    int sy = h / HACK_H;
    int s = (sx < sy) ? sx : sy;
    if (s < 1) s = 1;
    if (s > 3) s = 3; /* tables precalculees jusqu'a x3 */
    return s;
}

static void setup_hack_viewport(int w, int h) {
    g_hack_scale = clamp_hack_scale(w, h);
    g_view_w = HACK_W * g_hack_scale;
    g_view_h = HACK_H * g_hack_scale;
    g_view_x = (w - g_view_w) / 2;
    g_view_y = (h - g_view_h) / 2;
    if (g_view_x < 0) g_view_x = 0;
    if (g_view_y < 0) g_view_y = 0;
}

static void gfx_add_mode_candidate(int *mode_w, int *mode_h, int *mode_n, int w, int h) {
    if (w < 320 || h < 200) return;
    for (int i = 0; i < *mode_n; i++) {
        if (mode_w[i] == w && mode_h[i] == h) return;
    }
    if (*mode_n >= 10) return;
    mode_w[*mode_n] = w;
    mode_h[*mode_n] = h;
    (*mode_n)++;
}

void gfx_set_preferred_resolution(uint32_t width, uint32_t height) {
    if (width < 320 || height < 200 || width > 4096 || height > 2160) {
        g_pref_w = 0;
        g_pref_h = 0;
        return;
    }
    g_pref_w = (int)width;
    g_pref_h = (int)height;
}

static int vbe_try_enable_mode(int w, int h) {
    uint16_t id = vbe_read(VBE_DISPI_INDEX_ID);
    if (id < 0xB0C0 || id > 0xB0FF) return 0;

    /* Select highest known interface revision and program mode. */
    vbe_write(VBE_DISPI_INDEX_ID, 0xB0C5);
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    vbe_write(VBE_DISPI_INDEX_XRES, (uint16_t)w);
    vbe_write(VBE_DISPI_INDEX_YRES, (uint16_t)h);
    vbe_write(VBE_DISPI_INDEX_BPP, 8);
    /* Use banked window at A0000 for maximal compatibility in QEMU boot flow. */
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED);

    uint16_t rw = vbe_read(VBE_DISPI_INDEX_XRES);
    uint16_t rh = vbe_read(VBE_DISPI_INDEX_YRES);
    uint16_t rb = vbe_read(VBE_DISPI_INDEX_BPP);
    if ((int)rw != w || (int)rh != h || rb != 8) {
        vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        return 0;
    }

    uint16_t virt_w = vbe_read(VBE_DISPI_INDEX_VIRT_W);
    g_vbe_w = w;
    g_vbe_pitch = virt_w ? (int)virt_w : w;
    g_vbe_h = h;
    vbe_set_bank(0);
    return 1;
}

void gfx_capture_text_mode(void) {
    g_saved_misc = inb(VGA_MISC_READ);

    for (uint8_t i = 0; i < 5; i++) {
        outb(VGA_SEQ_INDEX, i);
        g_saved_seq[i] = inb(VGA_SEQ_DATA);
    }
    for (uint8_t i = 0; i < 25; i++) {
        outb(VGA_CRTC_INDEX, i);
        g_saved_crtc[i] = inb(VGA_CRTC_DATA);
    }
    for (uint8_t i = 0; i < 9; i++) {
        outb(VGA_GC_INDEX, i);
        g_saved_gc[i] = inb(VGA_GC_DATA);
    }
    for (uint8_t i = 0; i < 21; i++) {
        (void)inb(VGA_INSTAT_READ);
        outb(VGA_AC_INDEX, i);
        g_saved_ac[i] = inb(VGA_AC_READ);
    }
    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, 0x20);

    g_saved_valid = 1;
    g_backend = 0;
    g_vbe_w = VBE_X3_W;
    g_vbe_pitch = VBE_X3_W;
    g_vbe_h = VBE_X3_H;
    g_hack_scale = 3;
    g_view_x = 0;
    g_view_y = 0;
    g_view_w = VBE_X3_W;
    g_view_h = VBE_X3_H;
    g_graphics = 0;
    vga_capture_text_font();
}

void gfx_set_mode13(void) {
    int mode_w[10] = {0};
    int mode_h[10] = {0};
    int mode_n = 0;
    int pref_scale = 0;
    int exact_w = VBE_X3_W;
    int exact_h = VBE_X3_H;

    if (g_pref_w > 0 && g_pref_h > 0) {
        pref_scale = clamp_hack_scale(g_pref_w, g_pref_h);
        exact_w = HACK_W * pref_scale;
        exact_h = HACK_H * pref_scale;
        if (exact_w > g_pref_w || exact_h > g_pref_h) {
            exact_w = g_pref_w;
            exact_h = g_pref_h;
        }
    } else {
        pref_scale = 3;
    }

    /* Prefer exact integer-scaled surfaces first for a sharper, pixel-perfect window. */
    gfx_add_mode_candidate(mode_w, mode_h, &mode_n, exact_w, exact_h);
    if (pref_scale >= 3) {
        gfx_add_mode_candidate(mode_w, mode_h, &mode_n, VBE_X2_W, VBE_X2_H);
    }
    if (g_pref_w > 0 && g_pref_h > 0) {
        gfx_add_mode_candidate(mode_w, mode_h, &mode_n, g_pref_w, g_pref_h);
    }
    gfx_add_mode_candidate(mode_w, mode_h, &mode_n, VBE_X3_W, VBE_X3_H);
    gfx_add_mode_candidate(mode_w, mode_h, &mode_n, VBE_X2_W, VBE_X2_H);
    gfx_add_mode_candidate(mode_w, mode_h, &mode_n, 1366, 768);
    gfx_add_mode_candidate(mode_w, mode_h, &mode_n, 1280, 720);
    gfx_add_mode_candidate(mode_w, mode_h, &mode_n, 1024, 768);
    gfx_add_mode_candidate(mode_w, mode_h, &mode_n, 1600, 900);
    gfx_add_mode_candidate(mode_w, mode_h, &mode_n, 1920, 1080);

    for (int i = 0; i < mode_n; i++) {
        if (!vbe_try_enable_mode(mode_w[i], mode_h[i])) continue;

        setup_hack_viewport(g_vbe_w, g_vbe_h);
        gfx_init_expand8();
        vga_dac_unmask();
        /* Palette minimaliste: 0=black, 1=white */
        vga_dac_write(0, 0x00, 0x00, 0x00);
        vga_dac_write(1, 0x3F, 0x3F, 0x3F);
        g_backend = 1;
        g_graphics = 1;
        g_shadow_valid = 0;
        gfx_clear(0);
        return;
    }

    /* Fallback: VGA mode13 (320x200). */
    vga_set_regs(g_mode13_misc, g_mode13_seq, g_mode13_crtc, g_mode13_gc, g_mode13_ac);

    /* Force linear chain-4 mapping (mode 13h semantics). */
    outb(VGA_SEQ_INDEX, 0x02); outb(VGA_SEQ_DATA, 0x0F); /* map mask all planes */
    outb(VGA_SEQ_INDEX, 0x04); outb(VGA_SEQ_DATA, 0x0E); /* ext mem + chain4 */
    outb(VGA_GC_INDEX,  0x04); outb(VGA_GC_DATA,  0x00); /* read map 0 */
    outb(VGA_GC_INDEX,  0x05); outb(VGA_GC_DATA,  0x40); /* shift 256 */
    outb(VGA_GC_INDEX,  0x06); outb(VGA_GC_DATA,  0x05); /* A000 64K */

    vga_dac_unmask();
    vga_dac_write(0, 0x00, 0x00, 0x00);
    vga_dac_write(1, 0x3F, 0x3F, 0x3F);

    g_backend = 2;
    g_graphics = 1;
    g_hack_scale = 1;
    g_view_x = 0;
    g_view_y = 0;
    g_view_w = GFX_W;
    g_view_h = GFX_H;
    g_shadow_valid = 0;
}

void gfx_restore_text_mode(void) {
    /* Toujours tenter de desactiver VBE pour eviter un retour noir. */
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    (void)g_saved_valid;
    (void)g_saved_misc;
    (void)g_saved_seq;
    (void)g_saved_crtc;
    (void)g_saved_gc;
    (void)g_saved_ac;

    /* Reprogrammation explicite en mode texte VGA 80x25. */
    vga_set_regs(g_mode3_misc, g_mode3_seq, g_mode3_crtc, g_mode3_gc, g_mode3_ac);
    vga_dac_unmask();
    vga_load_text_palette_16();
    vga_restore_text_font();
    g_backend = 0;
    g_graphics = 0;
    g_hack_scale = 3;
    g_view_x = 0;
    g_view_y = 0;
    g_view_w = VBE_X3_W;
    g_view_h = VBE_X3_H;
    g_shadow_valid = 0;
}

int gfx_is_graphics(void) {
    return g_graphics;
}

void gfx_clear(uint8_t color) {
    g_shadow_valid = 0;
    if (g_backend == 1) {
        int total = g_vbe_pitch * g_vbe_h;
        int done = 0;
        uint16_t bank = 0;
        while (done < total) {
            int chunk = total - done;
            if (chunk > 65536) chunk = 65536;
            vbe_set_bank(bank++);
            for (int i = 0; i < chunk; i++) g_fb13[i] = color;
            done += chunk;
        }
        vbe_set_bank(0);
        return;
    }
    for (int i = 0; i < GFX_W * GFX_H; i++) {
        g_fb13[i] = color;
    }
}

void gfx_set_palette(uint8_t idx, uint8_t r6, uint8_t g6, uint8_t b6) {
    if (r6 > 63) r6 = 63;
    if (g6 > 63) g6 = 63;
    if (b6 > 63) b6 = 63;
    vga_dac_unmask();
    vga_dac_write(idx, r6, g6, b6);
}

void gfx_blit_indexed_512x256(const uint8_t *src) {
    if (!src || !g_graphics) return;

    gfx_wait_vblank();

    if (g_backend == 1) {
        uint16_t cur_bank = 0xFFFF;
        int scale = g_hack_scale;
        int row_len = HACK_W * scale;

        for (int y = 0; y < HACK_H; y++) {
            const uint8_t *in = src + y * HACK_W;
            int ys = g_view_y + y * scale;

            if (scale == 1) {
                for (int dy = 0; dy < 1; dy++) {
                    uint32_t off = (uint32_t)((ys + dy) * g_vbe_pitch + g_view_x);
                    vbe_mem_write(off, in, HACK_W, &cur_bank);
                }
            } else if (scale == 2) {
                int p = 0;
                for (int x = 0; x < HACK_W; x++) {
                    uint8_t c = in[x];
                    g_scale_row[p++] = c;
                    g_scale_row[p++] = c;
                }
                for (int dy = 0; dy < 2; dy++) {
                    uint32_t off = (uint32_t)((ys + dy) * g_vbe_pitch + g_view_x);
                    vbe_mem_write(off, g_scale_row, row_len, &cur_bank);
                }
            } else {
                int p = 0;
                for (int x = 0; x < HACK_W; x++) {
                    uint8_t c = in[x];
                    g_scale_row[p++] = c;
                    g_scale_row[p++] = c;
                    g_scale_row[p++] = c;
                }
                for (int dy = 0; dy < 3; dy++) {
                    uint32_t off = (uint32_t)((ys + dy) * g_vbe_pitch + g_view_x);
                    vbe_mem_write(off, g_scale_row, row_len, &cur_bank);
                }
            }
        }
        vbe_set_bank(0);
        return;
    }

    /* Fallback VGA mode13: remap nearest-neighbor vers 320x200. */
    for (int y = 0; y < GFX_H; y++) {
        int sy = (y * HACK_H) / GFX_H;
        const uint8_t *in = src + sy * HACK_W;
        for (int x = 0; x < GFX_W; x++) {
            int sx = (x * HACK_W) / GFX_W;
            g_fb13[y * GFX_W + x] = in[sx];
        }
    }
}

void gfx_blit_indexed_512x256_rect(const uint8_t *src, int sx, int sy, int w, int h) {
    if (!src || !g_graphics) return;
    if (sx < 0) { w += sx; sx = 0; }
    if (sy < 0) { h += sy; sy = 0; }
    if (sx >= HACK_W || sy >= HACK_H || w <= 0 || h <= 0) return;
    if (sx + w > HACK_W) w = HACK_W - sx;
    if (sy + h > HACK_H) h = HACK_H - sy;
    if (w <= 0 || h <= 0) return;

    if (g_backend == 1) {
        uint16_t cur_bank = 0xFFFF;
        int scale = g_hack_scale;
        int row_len = w * scale;

        for (int y = sy; y < sy + h; y++) {
            const uint8_t *in = src + y * HACK_W + sx;
            int ys = g_view_y + y * scale;
            int xs = g_view_x + sx * scale;

            if (scale == 1) {
                for (int dy = 0; dy < 1; dy++) {
                    uint32_t off = (uint32_t)((ys + dy) * g_vbe_pitch + xs);
                    vbe_mem_write(off, in, w, &cur_bank);
                }
            } else if (scale == 2) {
                int p = 0;
                for (int x = 0; x < w; x++) {
                    uint8_t c = in[x];
                    g_scale_row[p++] = c;
                    g_scale_row[p++] = c;
                }
                for (int dy = 0; dy < 2; dy++) {
                    uint32_t off = (uint32_t)((ys + dy) * g_vbe_pitch + xs);
                    vbe_mem_write(off, g_scale_row, row_len, &cur_bank);
                }
            } else {
                int p = 0;
                for (int x = 0; x < w; x++) {
                    uint8_t c = in[x];
                    g_scale_row[p++] = c;
                    g_scale_row[p++] = c;
                    g_scale_row[p++] = c;
                }
                for (int dy = 0; dy < 3; dy++) {
                    uint32_t off = (uint32_t)((ys + dy) * g_vbe_pitch + xs);
                    vbe_mem_write(off, g_scale_row, row_len, &cur_bank);
                }
            }
        }
        vbe_set_bank(0);
        return;
    }

    /* Fallback VGA mode13: update the destination area overlapping the source rect. */
    {
        int dx0 = (sx * GFX_W) / HACK_W;
        int dx1 = ((sx + w) * GFX_W + HACK_W - 1) / HACK_W;
        int dy0 = (sy * GFX_H) / HACK_H;
        int dy1 = ((sy + h) * GFX_H + HACK_H - 1) / HACK_H;
        if (dx0 < 0) dx0 = 0;
        if (dy0 < 0) dy0 = 0;
        if (dx1 > GFX_W) dx1 = GFX_W;
        if (dy1 > GFX_H) dy1 = GFX_H;
        for (int y = dy0; y < dy1; y++) {
            int src_y = (y * HACK_H) / GFX_H;
            const uint8_t *row = src + src_y * HACK_W;
            for (int x = dx0; x < dx1; x++) {
                int src_x = (x * HACK_W) / GFX_W;
                g_fb13[y * GFX_W + x] = row[src_x];
            }
        }
    }
}

/* Rendu 1bpp Hack -> mode13 avec ratio preserve (320x160 centré verticalement). */
void gfx_blit_hack_screen(const int16_t *hack_screen_base, int first_word, int last_word) {
    if (g_backend == 1) {
        /* VBE banked A0000: Hack pixels scaled x1/x2/x3 (pixel-perfect). */
        if (first_word < 0 || last_word < first_word) {
            first_word = 0;
            last_word = (HACK_H * HACK_WORDS_PER_ROW) - 1;
        }
        if (first_word < 0) first_word = 0;
        if (last_word > (HACK_H * HACK_WORDS_PER_ROW) - 1) {
            last_word = (HACK_H * HACK_WORDS_PER_ROW) - 1;
        }

        int force_write = !g_shadow_valid;
        g_shadow_valid = 1;

        uint16_t cur_bank = 0xFFFF;
        int scale = g_hack_scale;
        int half_len = scale * 8;
        for (int wi = first_word; wi <= last_word; wi++) {
            uint16_t w = (uint16_t)hack_screen_base[wi];
            if (!force_write && g_shadow_words[wi] == w) continue;
            g_shadow_words[wi] = w;

            int y  = wi >> 5;
            int xw = wi & 31;
            int ys = g_view_y + y * scale;
            int xs = g_view_x + xw * 16 * scale;
            const uint8_t *l;
            const uint8_t *h;
            if (scale == 3) {
                l = g_expand8x3[w & 0xFF];
                h = g_expand8x3[(w >> 8) & 0xFF];
            } else if (scale == 2) {
                l = g_expand8x2[w & 0xFF];
                h = g_expand8x2[(w >> 8) & 0xFF];
            } else {
                l = g_expand8x1[w & 0xFF];
                h = g_expand8x1[(w >> 8) & 0xFF];
            }

            for (int dy = 0; dy < scale; dy++) {
                uint32_t off = (uint32_t)((ys + dy) * g_vbe_pitch + xs);
                vbe_mem_write(off, l, half_len, &cur_bank);
                vbe_mem_write(off + (uint32_t)half_len, h, half_len, &cur_bank);
            }
        }
        vbe_set_bank(0);
        return;
    }

    (void)first_word;
    (void)last_word;

    const int view_h = 160;
    const int y_off  = (GFX_H - view_h) / 2;

    for (int y = 0; y < GFX_H; y++) {
        if (y < y_off || y >= (y_off + view_h)) {
            for (int x = 0; x < GFX_W; x++) g_fb13[y * GFX_W + x] = 0; /* black border */
            continue;
        }

        for (int x = 0; x < GFX_W; x++) {
            int sx0 = (x * HACK_W) / GFX_W;
            int sx1 = ((x + 1) * HACK_W) / GFX_W;
            int sy0 = ((y - y_off) * HACK_H) / view_h;
            int sy1 = (((y - y_off) + 1) * HACK_H) / view_h;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sy1 <= sy0) sy1 = sy0 + 1;

            int black = 0, total = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                int row = sy * HACK_WORDS_PER_ROW;
                for (int sx = sx0; sx < sx1; sx++) {
                    int wi = row + (sx >> 4);
                    int bt = sx & 15;
                    uint16_t w = (uint16_t)hack_screen_base[wi];
                    int bit = (w >> bt) & 1; /* 1=black, 0=white */
                    black += bit;
                    total++;
                }
            }
            g_fb13[y * GFX_W + x] = (black * 2 >= total) ? 0 : 1;
        }
    }
}
