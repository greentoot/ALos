#ifndef DRIVER_GFX_H
#define DRIVER_GFX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Capture l'etat texte courant (a appeler au boot). */
void gfx_capture_text_mode(void);

/* Resolution preferee pour le mode graphique (optionnel). */
void gfx_set_preferred_resolution(uint32_t width, uint32_t height);

/* Bascule en mode graphique VGA 320x200x8bpp (mode 13h-like). */
void gfx_set_mode13(void);

/* Restaure le mode texte capture au boot. */
void gfx_restore_text_mode(void);

/* Etat courant du driver graphique. */
int  gfx_is_graphics(void);

/* Outils framebuffer mode13. */
void gfx_clear(uint8_t color);
/* first_word/last_word: range [0..8191], pass negatives for full redraw */
void gfx_blit_hack_screen(const int16_t *hack_screen_base, int first_word, int last_word);

/* Palette DAC 6 bits par canal (0..63). */
void gfx_set_palette(uint8_t idx, uint8_t r6, uint8_t g6, uint8_t b6);

/* Blit couleur 8bpp source 512x256 vers la zone graphique active. */
void gfx_blit_indexed_512x256(const uint8_t *src);

/* Blit partiel couleur 8bpp source 512x256 vers la zone graphique active. */
void gfx_blit_indexed_512x256_rect(const uint8_t *src, int sx, int sy, int w, int h);

#ifdef __cplusplus
}
#endif

#endif
