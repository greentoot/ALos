#include "serial.h"

#include <stdint.h>

#define COM1_PORT 0x3F8

static int g_serial_ready = 0;

static inline void serial_outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t serial_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static int serial_tx_empty(void) {
    return (serial_inb(COM1_PORT + 5) & 0x20) != 0;
}

static int serial_rx_ready(void) {
    return (serial_inb(COM1_PORT + 5) & 0x01) != 0;
}

void serial_init(void) {
    serial_outb(COM1_PORT + 1, 0x00);
    serial_outb(COM1_PORT + 3, 0x80);
    serial_outb(COM1_PORT + 0, 0x03);
    serial_outb(COM1_PORT + 1, 0x00);
    serial_outb(COM1_PORT + 3, 0x03);
    serial_outb(COM1_PORT + 2, 0xC7);
    serial_outb(COM1_PORT + 4, 0x0B);
    g_serial_ready = 1;
}

int serial_is_ready(void) {
    return g_serial_ready;
}

int serial_char_available(void) {
    return g_serial_ready && serial_rx_ready();
}

int serial_read_char(char *out) {
    if (!g_serial_ready || !out || !serial_rx_ready()) return 0;
    *out = (char)serial_inb(COM1_PORT);
    return 1;
}

void serial_write_char(char c) {
    if (!g_serial_ready) return;
    while (!serial_tx_empty()) { }
    serial_outb(COM1_PORT, (uint8_t)c);
}

void serial_write(const char *s) {
    if (!g_serial_ready || !s) return;
    while (*s) {
        if (*s == '\n') serial_write_char('\r');
        serial_write_char(*s++);
    }
}

void serial_write_line(const char *s) {
    serial_write(s);
    serial_write("\n");
}
