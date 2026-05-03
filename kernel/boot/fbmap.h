#ifndef KERNEL_BOOT_FBMAP_H
#define KERNEL_BOOT_FBMAP_H

#include <stdint.h>
#include "bootinfo.h"

uint64_t fbmap_prepare_framebuffer(const BootInfo *bi);

#endif
