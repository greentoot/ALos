#include "mouse.h"
#include "keyboard.h"

static inline uint8_t inb(uint16_t port) {
    uint8_t v; __asm__ volatile ("inb %1,%0" : "=a"(v) : "Nd"(port)); return v;
}

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(port));
}

static volatile int g_mouse_ready = 0;
static volatile int g_mouse_present = 0;
static volatile int g_mouse_x = 256;
static volatile int g_mouse_y = 128;
static volatile int g_mouse_w = 512;
static volatile int g_mouse_h = 256;
static volatile uint8_t g_mouse_buttons = 0;
static volatile uint8_t g_mouse_packet[3];
static volatile uint8_t g_mouse_packet_pos = 0;

static int ps2_wait_write(void) {
    for (int i = 0; i < 200000; ++i) {
        if ((inb(KBD_STATUS_PORT) & 0x02) == 0) return 1;
    }
    return 0;
}

static int ps2_wait_read(void) {
    for (int i = 0; i < 200000; ++i) {
        if (inb(KBD_STATUS_PORT) & 0x01) return 1;
    }
    return 0;
}

static int ps2_read_data(uint8_t *out) {
    if (!out || !ps2_wait_read()) return 0;
    *out = inb(KBD_DATA_PORT);
    return 1;
}

static int ps2_write_mouse(uint8_t value) {
    uint8_t ack = 0;
    if (!ps2_wait_write()) return 0;
    outb(KBD_STATUS_PORT, 0xD4);
    if (!ps2_wait_write()) return 0;
    outb(KBD_DATA_PORT, value);
    if (!ps2_read_data(&ack)) return 0;
    return ack == 0xFA;
}

static void mouse_clamp_position(void) {
    int max_x = g_mouse_w > 0 ? (g_mouse_w - 1) : 0;
    int max_y = g_mouse_h > 0 ? (g_mouse_h - 1) : 0;
    if (g_mouse_x < 0) g_mouse_x = 0;
    if (g_mouse_y < 0) g_mouse_y = 0;
    if (g_mouse_x > max_x) g_mouse_x = max_x;
    if (g_mouse_y > max_y) g_mouse_y = max_y;
}

void mouse_set_bounds(int width, int height) {
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    g_mouse_w = width;
    g_mouse_h = height;
    mouse_clamp_position();
}

void mouse_set_position(int x, int y) {
    g_mouse_x = x;
    g_mouse_y = y;
    mouse_clamp_position();
}

int mouse_is_ready(void) {
    return g_mouse_ready;
}

int mouse_is_present(void) {
    return g_mouse_present;
}

void mouse_get_state(MouseState *out_state) {
    if (!out_state) return;
    out_state->x = g_mouse_x;
    out_state->y = g_mouse_y;
    out_state->buttons = g_mouse_buttons;
    out_state->ready = (uint8_t)g_mouse_ready;
    out_state->present = (uint8_t)g_mouse_present;
}

void mouse_irq_handler(void) {
    uint8_t status = inb(KBD_STATUS_PORT);
    uint8_t data = 0;

    if ((status & 0x01) == 0) goto done;
    data = inb(KBD_DATA_PORT);

    if ((status & 0x20) == 0) goto done;

    if (g_mouse_packet_pos == 0 && (data & 0x08) == 0) goto done;
    g_mouse_packet[g_mouse_packet_pos++] = data;
    if (g_mouse_packet_pos >= 3) {
        uint8_t b0 = g_mouse_packet[0];
        uint8_t b1 = g_mouse_packet[1];
        uint8_t b2 = g_mouse_packet[2];
        g_mouse_packet_pos = 0;

        if ((b0 & 0xC0) == 0) {
            int dx = (b0 & 0x10) ? ((int)b1 - 256) : (int)b1;
            int dy = (b0 & 0x20) ? ((int)b2 - 256) : (int)b2;
            g_mouse_x += dx;
            g_mouse_y -= dy;
            mouse_clamp_position();
            g_mouse_buttons = (uint8_t)(b0 & 0x07);
            g_mouse_present = 1;
        }
    }

done:
    outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void mouse_init(void) {
    uint8_t status = 0;

    g_mouse_ready = 0;
    g_mouse_present = 0;
    g_mouse_packet_pos = 0;
    g_mouse_buttons = 0;
    mouse_set_bounds(512, 256);
    mouse_set_position(384, 110);

    if (!ps2_wait_write()) return;
    outb(KBD_STATUS_PORT, 0xA8);

    if (!ps2_wait_write()) return;
    outb(KBD_STATUS_PORT, 0x20);
    if (!ps2_read_data(&status)) return;

    status |= 0x02;
    status &= (uint8_t)~0x20;

    if (!ps2_wait_write()) return;
    outb(KBD_STATUS_PORT, 0x60);
    if (!ps2_wait_write()) return;
    outb(KBD_DATA_PORT, status);

    if (!ps2_write_mouse(0xF6)) return;
    if (!ps2_write_mouse(0xF4)) return;

    g_mouse_ready = 1;
}
