#ifndef KERNEL_BOOT_BOOTINFO_H
#define KERNEL_BOOT_BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_CMDLINE_MAX         128
#define BOOTINFO_MAX_MODULES         8
#define BOOTINFO_MODULE_CMDLINE_MAX  64

typedef struct {
    uint32_t start;
    uint32_t end;
    char     cmdline[BOOTINFO_MODULE_CMDLINE_MAX];
} BootModuleInfo;

typedef struct {
    uint8_t  multiboot_ok;
    uint32_t multiboot_flags;

    uint32_t mem_lower_kb;
    uint32_t mem_upper_kb;
    uint32_t mem_end_bytes;    /* usable memory ceiling used by PMM init */
    uint32_t mmap_entries;

    char     cmdline[BOOTINFO_CMDLINE_MAX];
    uint32_t modules_count;
    BootModuleInfo modules[BOOTINFO_MAX_MODULES];
    uint8_t  installer_requested;

    uint8_t  has_framebuffer;
    uint64_t fb_addr;
    uint32_t fb_pitch;
    uint32_t fb_width;
    uint32_t fb_height;
    uint8_t  fb_bpp;
    uint8_t  fb_type;
    uint8_t  fb_red_pos;
    uint8_t  fb_red_mask;
    uint8_t  fb_green_pos;
    uint8_t  fb_green_mask;
    uint8_t  fb_blue_pos;
    uint8_t  fb_blue_mask;

    uint8_t  hypervisor_present;
    char     hypervisor_vendor[13];
} BootInfo;

void bootinfo_parse(uint32_t multiboot_magic, uint32_t multiboot_info_addr);
const BootInfo *bootinfo_get(void);
void bootinfo_override_framebuffer_addr(uint64_t fb_addr);
const void *bootinfo_find_module(const char *cmdline, uint32_t *out_size);
const char *bootinfo_cmdline(void);
int bootinfo_installer_requested(void);

#endif
