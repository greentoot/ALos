#ifndef KEYBOARD_H
#define KEYBOARD_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KBD_DATA_PORT    0x60
#define KBD_STATUS_PORT  0x64
#define PIC1_CMD         0x20
#define PIC1_DATA        0x21
#define PIC2_CMD         0xA0
#define PIC2_DATA        0xA1
#define PIC_EOI          0x20

/* Keycodes spéciaux */
#define KEY_NONE         0x00
#define KEY_BACKSPACE    0x08
#define KEY_TAB          0x09
#define KEY_ENTER        0x0A
#define KEY_ESCAPE       0x1B
#define KEY_DELETE       0x7F

#define KEY_F1  0x80  
#define KEY_F2  0x81  
#define KEY_F3  0x82  
#define KEY_F4  0x83
#define KEY_F5  0x84  
#define KEY_F6  0x85  
#define KEY_F7  0x86  
#define KEY_F8  0x87
#define KEY_F9  0x88  
#define KEY_F10 0x89  
#define KEY_F11 0x8A  
#define KEY_F12 0x8B

#define KEY_UP     0x90  
#define KEY_DOWN   0x91  
#define KEY_LEFT   0x92  
#define KEY_RIGHT  0x93
#define KEY_HOME   0x94  
#define KEY_END    0x95  
#define KEY_PGUP   0x96  
#define KEY_PGDN   0x97
#define KEY_INSERT 0x98

/* Modificateurs */
#define MOD_SHIFT   (1<<0)
#define MOD_CTRL    (1<<1)
#define MOD_ALT     (1<<2)
#define MOD_CAPS    (1<<3)
#define MOD_NUMLOCK (1<<4)

#define KBD_BUFFER_SIZE 256

typedef struct {
    uint8_t  buf[KBD_BUFFER_SIZE];
    uint16_t head, tail, count;
} KbdBuffer;

void    keyboard_init(void);
void    keyboard_poll(void);
int     keyboard_available(void);
uint8_t keyboard_getkey(void);
void    keyboard_clear_buffer(void);
uint8_t keyboard_modifiers(void);
uint8_t keyboard_current_key(void); /* touche actuellement maintenue (0 si aucune) */
int     keyboard_ps2_present(void);
void    keyboard_irq_handler(void);
void    keyboard_inject_press(uint8_t key);
void    keyboard_inject_release(uint8_t key);
void    keyboard_inject_tap(uint8_t key);
void    keyboard_set_external_modifiers(uint8_t mods);

#ifdef __cplusplus
}
#endif

#endif
