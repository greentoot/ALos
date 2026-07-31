/**
 * kernel.c - ALOS v0.5
 */
#include "driver/vga.h"
#include "driver/gfx.h"
#include "driver/serial.h"
#include "driver/usb_probe.h"
#include "driver/usb_xhci.h"
#include "driver/timer.h"
#include "driver/keyboard.h"
#include "driver/usb_hid_kbd.h"
#include "kernel/tty.h"
#include "kernel/boot/bootinfo.h"
#include "kernel/boot/earlydiag.h"
#include "kernel/boot/fbmap.h"
#include "kernel/memory/pmm.h"
#include "kernel/memory/heap.h"
#include "kernel/process/task.h"
#include "kernel/process/scheduler.h"
#include "kernel/fs/ramfs.h"
#include "kernel/fs/vfs.h"
#include "kernel/lib/kprintf.h"
#include "kernel/shell.h"
#include <stdint.h>

#ifndef ALOS_STRICT_BAREMETAL
#define ALOS_STRICT_BAREMETAL 0
#endif

#ifndef ALOS_HW_SAFE
#define ALOS_HW_SAFE 0
#endif

extern uint32_t kernel_end;

#if ALOS_STRICT_BAREMETAL
static void kernel_halt_forever(void) {
    while (1) {
        __asm__ volatile ("cli; hlt");
    }
}
#endif

void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
    bootinfo_parse(multiboot_magic, multiboot_info_addr);
    const BootInfo *bi = bootinfo_get();
    uint64_t mapped_fb = fbmap_prepare_framebuffer(bi);
    if (mapped_fb && mapped_fb != bi->fb_addr) {
        bootinfo_override_framebuffer_addr(mapped_fb);
        bi = bootinfo_get();
    }
    earlydiag_init(bi);
    earlydiag_stage("bootinfo ok");
    earlydiag_stage(bi->has_framebuffer ? "framebuffer present" : "framebuffer absent");

#if ALOS_STRICT_BAREMETAL
    if (bi->hypervisor_present) {
        vga_print_at(0, 0, "ALOS: hyperviseur detecte, boot refuse (mode bare-metal strict).", VGA_COLOR(VGA_BLACK, VGA_LIGHT_RED));
        if (bi->hypervisor_vendor[0]) {
            vga_print_at(0, 1, "Hypervisor vendor:", VGA_COLOR(VGA_BLACK, VGA_YELLOW));
            vga_print_at(20, 1, bi->hypervisor_vendor, VGA_COLOR(VGA_BLACK, VGA_WHITE));
        }
        kernel_halt_forever();
    }
#endif

    if (bi->has_framebuffer && bi->fb_width && bi->fb_height) {
        gfx_set_preferred_resolution(bi->fb_width, bi->fb_height);
    } else {
        gfx_set_preferred_resolution(0, 0);
    }

    earlydiag_stage("tty init");
    tty_init();
    if (!tty_is_framebuffer()) {
        gfx_capture_text_mode();
    }
    earlydiag_stage(tty_is_framebuffer() ? "tty framebuffer/textfb ok" : "tty fallback vga");
    usb_probe_init();
#if ALOS_HW_SAFE
    {
        int usb_count = usb_probe_count();
        if (usb_count > 0) {
            char line[128];
            earlydiag_stage("usb probe ok");
            for (int i = 0; i < usb_count && i < 16; ++i) {
                const UsbHostControllerInfo *info = usb_probe_get(i);
                if (!info) continue;
                ksprintf(line,
                         "usb %s b%u s%u f%u irq%u vid=%x dev=%x pi=%x",
                         usb_probe_kind_name(info->kind),
                         (unsigned)info->bus,
                         (unsigned)info->slot,
                         (unsigned)info->func,
                         (unsigned)info->irq_line,
                         (unsigned)info->vendor,
                         (unsigned)info->device,
                         (unsigned)info->prog_if);
                earlydiag_stage(line);
                if (info->mmio_base && info->mmio_base <= 0xFFFFFFFFull) {
                    if (info->kind == USB_HC_XHCI) {
                        ksprintf(line,
                                 "  mmio=%x cap=%u ver=%x ports=%u slots=%u",
                                 (unsigned)(uint32_t)info->mmio_base,
                                 (unsigned)info->cap_length,
                                 (unsigned)info->hci_version,
                                 (unsigned)info->port_count,
                                 (unsigned)info->slot_count);
                    } else if (info->kind == USB_HC_EHCI) {
                        ksprintf(line,
                                 "  mmio=%x cap=%u ver=%x ports=%u",
                                 (unsigned)(uint32_t)info->mmio_base,
                                 (unsigned)info->cap_length,
                                 (unsigned)info->hci_version,
                                 (unsigned)info->port_count);
                    } else {
                        ksprintf(line,
                                 "  mmio=%x cap=%u ver=%x",
                                 (unsigned)(uint32_t)info->mmio_base,
                                 (unsigned)info->cap_length,
                                 (unsigned)info->hci_version);
                    }
                } else if (info->mmio_base) {
                    ksprintf(line,
                             "  mmio>4G cap=%u ver=%x",
                             (unsigned)info->cap_length,
                             (unsigned)info->hci_version);
                } else if (info->bar_is_io && info->io_base) {
                    ksprintf(line,
                             "  io=%x",
                             (unsigned)info->io_base);
                } else {
                    ksprintf(line, "  mmio=absent");
                }
                earlydiag_stage(line);
                if (info->legacy_cap_offset) {
                    ksprintf(line,
                             "  handoff cap=%x bios=%u os=%u ok=%u",
                             (unsigned)info->legacy_cap_offset,
                             (unsigned)info->legacy_bios_owned,
                             (unsigned)info->legacy_os_owned,
                             (unsigned)info->legacy_handoff_ok);
                    earlydiag_stage(line);
                }
            }
        } else {
            earlydiag_stage("usb probe: aucun controleur");
        }
    }
#endif

#if !ALOS_HW_SAFE
    serial_init();
#endif

    uint32_t mem_end = bi->mem_end_bytes;
    if (mem_end < (4u * 1024u * 1024u)) mem_end = 16u * 1024u * 1024u;

    earlydiag_stage("pmm init");
    pmm_init((uint32_t)&kernel_end, mem_end);
    earlydiag_stage("heap init");
    heap_init(0);
    earlydiag_stage("ramfs init");
    ramfs_init();
    earlydiag_stage("vfs init");
    vfs_init();
    earlydiag_stage("tasks init");
    tasks_init();
    earlydiag_stage("scheduler init");
    scheduler_init();
    earlydiag_stage("timer init");
    timer_init();
    earlydiag_stage("keyboard init");
    keyboard_init();
#if ALOS_HW_SAFE
    {
        char line[128];
        if (keyboard_ps2_present()) {
            earlydiag_stage("usb legacy keyboard preserved");
        }
        earlydiag_stage("usb xhci init");
        usb_xhci_init();
        usb_xhci_status_string(line, sizeof(line));
        earlydiag_stage(line);
        earlydiag_stage("usb hid init");
        usb_hid_kbd_init();
        usb_hid_kbd_status_string(line, sizeof(line));
        earlydiag_stage(line);
    }
#endif
    earlydiag_stage("interrupts ok");

    earlydiag_stage("shell run");
    shell_run();
}
