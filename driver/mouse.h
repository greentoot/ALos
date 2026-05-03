#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x;
    int y;
    uint8_t buttons;
    uint8_t ready;
    uint8_t present;
} MouseState;

void mouse_init(void);
void mouse_irq_handler(void);
void mouse_get_state(MouseState *out_state);
void mouse_set_bounds(int width, int height);
void mouse_set_position(int x, int y);
int  mouse_is_ready(void);
int  mouse_is_present(void);

#ifdef __cplusplus
}
#endif

#endif
