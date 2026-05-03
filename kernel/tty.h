#ifndef KERNEL_TTY_H
#define KERNEL_TTY_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Init : efface l'écran, dessine le séparateur et le prompt */
void tty_init(void);
int  tty_is_framebuffer(void);

/* Écrit str dans la zone scrollante, va à la ligne après */
void tty_write(const char *str);

/* Idem avec couleur explicite */
void tty_write_color(const char *str, uint8_t color);

/* Affiche "prompt + cmd" dans la zone scroll (historique commandes) */
void tty_echo_cmd(const char *str);

/* Efface toute la zone scrollante */
void tty_clear_output(void);

/* ── Ligne de saisie ───────────────────────────────────────────────────── */
void tty_clear_input(void);
void tty_echo_char(char c, int cursor_x);
void tty_backspace(int cursor_x);
int  tty_prompt_len(void);

/* Couleurs exposées pour usage ponctuel */
#define TTY_C_OUTPUT  0x07   /* blanc sur noir   */
#define TTY_C_OK      0x0A   /* vert clair       */
#define TTY_C_WARN    0x0E   /* jaune            */
#define TTY_C_ERR     0x0C   /* rouge clair      */
#define TTY_C_INFO    0x0B   /* cyan clair       */
#define TTY_C_MUTED   0x08   /* gris foncé       */

#ifdef __cplusplus
}
#endif

#endif
