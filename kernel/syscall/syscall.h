#ifndef KERNEL_SYSCALL_SYSCALL_H
#define KERNEL_SYSCALL_SYSCALL_H
#include <stdint.h>

/* Numéros syscall (eax lors de INT 0x80) */
#define SYS_WRITE   0
#define SYS_EXIT    1
#define SYS_GETPID  2
#define SYS_SLEEP   3
#define SYS_OPEN    4
#define SYS_READ    5
#define SYS_FREE    6

/* Handler C appelé par int80_stub */
uint32_t syscall_handler(uint32_t num,
                         uint32_t arg1,
                         uint32_t arg2,
                         uint32_t arg3);

#endif
