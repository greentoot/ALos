#ifndef DRIVER_VGA_H
#define DRIVER_VGA_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000

typedef enum {
    VGA_BLACK=0, VGA_BLUE, VGA_GREEN, VGA_CYAN, VGA_RED, VGA_MAGENTA,
    VGA_BROWN, VGA_LIGHT_GREY, VGA_DARK_GREY, VGA_LIGHT_BLUE,
    VGA_LIGHT_GREEN, VGA_LIGHT_CYAN, VGA_LIGHT_RED, VGA_LIGHT_MAGENTA,
    VGA_YELLOW, VGA_WHITE
} VGAColor;

#define VGA_COLOR(bg,fg) ((uint8_t)(((bg)<<4)|(fg)))

void vga_init(void);
void vga_clear(void);
void vga_put_char(int x, int y, char c, uint8_t color);
void vga_print_at(int x, int y, const char *str, uint8_t color);
void vga_scroll_region(int from_row, int to_row);
void vga_clear_line(int row, uint8_t color);

/* Affiche un entier sur 'width' colonnes (efface les résidus) */
void vga_pnum(int x, int y, int n, int width, uint8_t color);

#ifdef __cplusplus
}
#endif

#endif
