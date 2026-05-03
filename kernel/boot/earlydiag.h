#ifndef KERNEL_BOOT_EARLYDIAG_H
#define KERNEL_BOOT_EARLYDIAG_H

#include <stdint.h>
#include "bootinfo.h"

void earlydiag_init(const BootInfo *bi);
void earlydiag_stage(const char *msg);
void earlydiag_panic_num(const char *title, uint32_t code);

#endif
