/* driver/vga.c — VGA text-mode driver avec shadow buffer */
#include "vga.h"

static uint16_t shadow[VGA_WIDTH * VGA_HEIGHT];
static inline uint16_t *hw(void) { return (uint16_t *)VGA_MEMORY; }

void vga_init(void)  { vga_clear(); }

void vga_clear(void) {
    uint8_t  col  = VGA_COLOR(VGA_BLACK, VGA_WHITE);
    uint16_t cell = (uint16_t)((col << 8) | ' ');
    uint16_t *vga = hw();
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) { shadow[i]=cell; vga[i]=cell; }
}

void vga_put_char(int x, int y, char c, uint8_t color) {
    if ((unsigned)x >= VGA_WIDTH || (unsigned)y >= VGA_HEIGHT) return;
    uint16_t cell = (uint16_t)((color << 8) | (uint8_t)c);
    int idx = y * VGA_WIDTH + x;
    if (shadow[idx] == cell) return;
    shadow[idx] = cell; hw()[idx] = cell;
}

void vga_print_at(int x, int y, const char *str, uint8_t color) {
    while (*str) vga_put_char(x++, y, *str++, color);
}

void vga_scroll_region(int from_row, int to_row) {
    uint16_t *vga = hw();
    for (int y = from_row; y < to_row; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            int dst = y*VGA_WIDTH+x, src = dst+VGA_WIDTH;
            if (shadow[dst] != shadow[src]) { shadow[dst]=shadow[src]; vga[dst]=shadow[src]; }
        }
    }
    vga_clear_line(to_row, VGA_COLOR(VGA_BLACK, VGA_WHITE));
}

void vga_clear_line(int row, uint8_t color) {
    uint16_t blank = (uint16_t)((color << 8) | ' ');
    uint16_t *vga  = hw();
    for (int x = 0; x < VGA_WIDTH; x++) {
        int idx = row*VGA_WIDTH+x; shadow[idx]=blank; vga[idx]=blank;
    }
}

void vga_pnum(int x, int y, int n, int width, uint8_t color) {
    uint8_t blank = VGA_COLOR(VGA_BLACK, VGA_WHITE);
    char rev[12]; int rlen=0, cx=x;
    if (n < 0) { vga_put_char(cx++, y, '-', color); n=-n; width--; }
    if (n == 0) rev[rlen++]='0';
    else while (n>0) { rev[rlen++]='0'+(n%10); n/=10; }
    for (int i=rlen-1; i>=0; i--) vga_put_char(cx++, y, rev[i], color);
    int written = cx - x;
    while (written++ < width) vga_put_char(cx++, y, ' ', blank);
}
