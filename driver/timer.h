#ifndef DRIVER_TIMER_H
#define DRIVER_TIMER_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#define PIT_CHANNEL0  0x40
#define PIT_CMD       0x43
#define PIT_FREQ_BASE 1193180
#define TIMER_HZ      100
void     timer_init(void);
uint32_t timer_ticks(void);
uint32_t timer_ms(void);
void     timer_irq_handler(void);

#ifdef __cplusplus
}
#endif
#endif
