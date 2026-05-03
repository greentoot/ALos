#ifndef KERNEL_LIB_KPRINTF_H
#define KERNEL_LIB_KPRINTF_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* kprintf : écrit sur stdout du terminal kernel (tty0).
   Formats supportés : %d %u %x %s %c %% */
void kprintf(const char *fmt, ...);
/* ksprintf : comme kprintf mais dans un buffer */
int  ksprintf(char *buf, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif
