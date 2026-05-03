/* driver/timer.c ??? PIT 8253, 100 Hz */
#include "timer.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static volatile uint32_t _ticks = 0;

void timer_init(void) {
    uint32_t div = PIT_FREQ_BASE / TIMER_HZ;
    outb(PIT_CMD,      0x36);
    outb(PIT_CHANNEL0, (uint8_t)(div & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((div >> 8) & 0xFF));
}
uint32_t timer_ticks(void) { return _ticks; }
uint32_t timer_ms(void)    { return (uint32_t)(((uint64_t)_ticks * 1000ull) / (uint64_t)TIMER_HZ); }
void     timer_irq_handler(void) { _ticks++; }

