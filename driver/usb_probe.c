#include "usb_probe.h"

#ifndef ALOS_HW_SAFE
#define ALOS_HW_SAFE 0
#endif

#define USB_PROBE_MAX 32

static UsbHostControllerInfo g_usb_hc[USB_PROBE_MAX];
static int g_usb_hc_count = 0;

static inline void outl(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0,%1" :: "a"(v), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile ("inl %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address =
        0x80000000u |
        ((uint32_t)bus << 16) |
        ((uint32_t)slot << 11) |
        ((uint32_t)func << 8) |
        (offset & 0xFCu);
    outl(0xCF8, address);
    return inl(0xCFC);
}

static void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address =
        0x80000000u |
        ((uint32_t)bus << 16) |
        ((uint32_t)slot << 11) |
        ((uint32_t)func << 8) |
        (offset & 0xFCu);
    outl(0xCF8, address);
    outl(0xCFC, value);
}

static uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t value = pci_config_read32(bus, slot, func, offset);
    uint8_t shift = (uint8_t)((offset & 3u) * 8u);
    return (uint8_t)((value >> shift) & 0xFFu);
}

static uint64_t pci_read_bar64(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_offset) {
    uint32_t lo = pci_config_read32(bus, slot, func, bar_offset);
    if ((lo & 0x1u) != 0) return 0; /* I/O BAR not handled */

    {
        uint32_t type = (lo >> 1) & 0x3u;
        if (type == 0x2u) {
            uint32_t hi = pci_config_read32(bus, slot, func, (uint8_t)(bar_offset + 4u));
            return (((uint64_t)hi) << 32) | (uint64_t)(lo & ~0xFu);
        }
    }

    return (uint64_t)(lo & ~0xFu);
}

static uint16_t pci_read_bar_io(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_offset) {
    uint32_t lo = pci_config_read32(bus, slot, func, bar_offset);
    if ((lo & 0x1u) == 0) return 0;
    return (uint16_t)(lo & ~0x3u);
}

static uint8_t pci_bar_is_io(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_offset) {
    return (pci_config_read32(bus, slot, func, bar_offset) & 0x1u) ? 1u : 0u;
}

static void pci_enable_controller(uint8_t bus, uint8_t slot, uint8_t func, uint8_t needs_io) {
    uint32_t cmd_status = pci_config_read32(bus, slot, func, 0x04);
    uint32_t cmd = cmd_status & 0xFFFFu;
    cmd |= (1u << 2); /* bus master */
    cmd |= needs_io ? 1u : (1u << 1);
    pci_config_write32(bus, slot, func, 0x04, (cmd_status & 0xFFFF0000u) | cmd);
}

static void usb_probe_mmio_caps(UsbHostControllerInfo *info) {
    volatile uint8_t *mmio;
    uint32_t hcs_params;

    if (!info || !info->mmio_base || info->mmio_base > 0xFFFFFFFFull) return;

    mmio = (volatile uint8_t *)(uintptr_t)info->mmio_base;
    info->cap_length = mmio[0];
    info->hci_version = *(volatile uint16_t *)(const void *)(mmio + 2);
    hcs_params = *(volatile uint32_t *)(const void *)(mmio + 4);

    if (info->kind == USB_HC_XHCI) {
        info->slot_count = (uint8_t)(hcs_params & 0xFFu);
        info->port_count = (uint8_t)((hcs_params >> 24) & 0xFFu);
    } else if (info->kind == USB_HC_EHCI) {
        info->slot_count = 0;
        info->port_count = (uint8_t)(hcs_params & 0x0Fu);
    } else if (info->kind == USB_HC_OHCI) {
        uint32_t rh_desc_a = *(volatile uint32_t *)(const void *)(mmio + 0x48);
        info->cap_length = 0;
        info->hci_version = (uint16_t)mmio[0];
        info->slot_count = 0;
        info->port_count = (uint8_t)(rh_desc_a & 0xFFu);
    } else {
        info->slot_count = 0;
        info->port_count = 0;
    }
}

static void cpu_relax_delay(void) {
    for (volatile uint32_t i = 0; i < 200000u; ++i) {
        __asm__ volatile ("pause");
    }
}

static void usb_probe_xhci_handoff(UsbHostControllerInfo *info) {
    volatile uint8_t *mmio;
    uint32_t hccparams1;
    uint32_t xecp_dwords;
    uint32_t off;

    if (!info || info->kind != USB_HC_XHCI || !info->mmio_base || info->mmio_base > 0xFFFFFFFFull) return;

    mmio = (volatile uint8_t *)(uintptr_t)info->mmio_base;
    hccparams1 = *(volatile uint32_t *)(const void *)(mmio + 0x10);
    xecp_dwords = (hccparams1 >> 16) & 0xFFFFu;
    off = xecp_dwords << 2;

    while (off >= 0x10u && off < 0x1000u) {
        volatile uint32_t *reg = (volatile uint32_t *)(const void *)(mmio + off);
        uint32_t value = *reg;
        uint8_t cap_id = (uint8_t)(value & 0xFFu);
        uint8_t next = (uint8_t)((value >> 8) & 0xFFu);

        if (cap_id == 0u) break;
        if (cap_id == 1u) {
            uint32_t v = *reg;
            info->legacy_cap_offset = (uint16_t)off;
            info->legacy_bios_owned = (uint8_t)((v >> 16) & 1u);
            info->legacy_os_owned = (uint8_t)((v >> 24) & 1u);
            if (!info->legacy_os_owned) {
                v |= (1u << 24);
                *reg = v;
            }
            for (int attempt = 0; attempt < 200; ++attempt) {
                v = *reg;
                info->legacy_bios_owned = (uint8_t)((v >> 16) & 1u);
                info->legacy_os_owned = (uint8_t)((v >> 24) & 1u);
                if (!info->legacy_bios_owned && info->legacy_os_owned) {
                    info->legacy_handoff_ok = 1;
                    return;
                }
                cpu_relax_delay();
            }
            info->legacy_handoff_ok = (!info->legacy_bios_owned && info->legacy_os_owned) ? 1 : 0;
            return;
        }

        if (next == 0u) break;
        off += ((uint32_t)next << 2);
    }
}

static void usb_probe_ehci_handoff(UsbHostControllerInfo *info) {
    volatile uint8_t *mmio;
    uint32_t hccparams;
    uint8_t eecp;

    if (!info || info->kind != USB_HC_EHCI || !info->mmio_base || info->mmio_base > 0xFFFFFFFFull) return;

    mmio = (volatile uint8_t *)(uintptr_t)info->mmio_base;
    hccparams = *(volatile uint32_t *)(const void *)(mmio + 0x08);
    eecp = (uint8_t)((hccparams >> 8) & 0xFFu);
    if (eecp < 0x40u) return;

    {
        uint32_t value = pci_config_read32(info->bus, info->slot, info->func, eecp);
        info->legacy_cap_offset = eecp;
        info->legacy_bios_owned = (uint8_t)((value >> 16) & 1u);
        info->legacy_os_owned = (uint8_t)((value >> 24) & 1u);
        if (!info->legacy_os_owned) {
            value |= (1u << 24);
            pci_config_write32(info->bus, info->slot, info->func, eecp, value);
        }

        for (int attempt = 0; attempt < 200; ++attempt) {
            value = pci_config_read32(info->bus, info->slot, info->func, eecp);
            info->legacy_bios_owned = (uint8_t)((value >> 16) & 1u);
            info->legacy_os_owned = (uint8_t)((value >> 24) & 1u);
            if (!info->legacy_bios_owned && info->legacy_os_owned) {
                info->legacy_handoff_ok = 1;
                return;
            }
            cpu_relax_delay();
        }
        info->legacy_handoff_ok = (!info->legacy_bios_owned && info->legacy_os_owned) ? 1 : 0;
    }
}

static uint8_t usb_prog_if_to_kind(uint8_t prog_if) {
    switch (prog_if) {
        case 0x00: return USB_HC_UHCI;
        case 0x10: return USB_HC_OHCI;
        case 0x20: return USB_HC_EHCI;
        case 0x30: return USB_HC_XHCI;
        default:   return USB_HC_OTHER;
    }
}

static uint8_t usb_known_amd_kind(uint16_t vendor, uint16_t device, uint8_t fallback_kind) {
    if (vendor != 0x1022u) return fallback_kind;
    switch (device) {
        case 0x7808u: return USB_HC_EHCI;
        case 0x7807u: return USB_HC_OHCI;
        case 0x7809u: return USB_HC_OHCI;
        case 0x7812u: return USB_HC_XHCI;
        case 0x7814u: return USB_HC_XHCI;
        default: return fallback_kind;
    }
}

static uint8_t usb_known_amd_device(uint16_t vendor, uint16_t device) {
    if (vendor != 0x1022u) return 0;
    switch (device) {
        case 0x7807u:
        case 0x7808u:
        case 0x7809u:
        case 0x7812u:
        case 0x7814u:
            return 1;
        default:
            return 0;
    }
}

const char *usb_probe_kind_name(uint8_t kind) {
    switch (kind) {
        case USB_HC_UHCI: return "UHCI";
        case USB_HC_OHCI: return "OHCI";
        case USB_HC_EHCI: return "EHCI";
        case USB_HC_XHCI: return "xHCI";
        case USB_HC_OTHER: return "USB";
        default: return "none";
    }
}

void usb_probe_init(void) {
    g_usb_hc_count = 0;
    for (int i = 0; i < USB_PROBE_MAX; ++i) {
        g_usb_hc[i].present = 0;
        g_usb_hc[i].kind = USB_HC_NONE;
        g_usb_hc[i].bus = 0;
        g_usb_hc[i].slot = 0;
        g_usb_hc[i].func = 0;
        g_usb_hc[i].vendor = 0xFFFFu;
        g_usb_hc[i].device = 0xFFFFu;
        g_usb_hc[i].prog_if = 0xFFu;
        g_usb_hc[i].irq_line = 0xFFu;
        g_usb_hc[i].cap_length = 0;
        g_usb_hc[i].hci_version = 0;
        g_usb_hc[i].port_count = 0;
        g_usb_hc[i].slot_count = 0;
        g_usb_hc[i].mmio_base = 0;
        g_usb_hc[i].io_base = 0;
        g_usb_hc[i].bar_is_io = 0;
        g_usb_hc[i].legacy_cap_offset = 0;
        g_usb_hc[i].legacy_bios_owned = 0;
        g_usb_hc[i].legacy_os_owned = 0;
        g_usb_hc[i].legacy_handoff_ok = 0;
    }

    for (uint16_t bus = 0; bus < 256 && g_usb_hc_count < USB_PROBE_MAX; ++bus) {
        for (uint8_t slot = 0; slot < 32 && g_usb_hc_count < USB_PROBE_MAX; ++slot) {
            for (uint8_t func = 0; func < 8u && g_usb_hc_count < USB_PROBE_MAX; ++func) {
                uint32_t vendor_device = pci_config_read32((uint8_t)bus, slot, func, 0x00);
                uint16_t vendor0 = (uint16_t)(vendor_device & 0xFFFFu);
                if (vendor0 == 0xFFFFu) continue;

                {
                    uint16_t device = (uint16_t)((vendor_device >> 16) & 0xFFFFu);
                    uint32_t class_reg = pci_config_read32((uint8_t)bus, slot, func, 0x08);
                    uint8_t class_code = (uint8_t)((class_reg >> 24) & 0xFFu);
                    uint8_t subclass = (uint8_t)((class_reg >> 16) & 0xFFu);
                    uint8_t prog_if = (uint8_t)((class_reg >> 8) & 0xFFu);

                    if ((class_code == 0x0Cu && subclass == 0x03u) ||
                        usb_known_amd_device(vendor0, device)) {
                        UsbHostControllerInfo *info = &g_usb_hc[g_usb_hc_count++];
                        info->present = 1;
                        info->kind = usb_known_amd_kind(vendor0, device, usb_prog_if_to_kind(prog_if));
                        info->bus = (uint8_t)bus;
                        info->slot = slot;
                        info->func = func;
                        info->vendor = vendor0;
                        info->device = device;
                        info->prog_if = prog_if;
                        info->irq_line = pci_config_read8((uint8_t)bus, slot, func, 0x3Cu);
                        {
                            uint8_t bar_offset = (info->kind == USB_HC_UHCI) ? 0x20u : 0x10u;
                            info->bar_is_io = pci_bar_is_io((uint8_t)bus, slot, func, bar_offset);
                            if (info->bar_is_io) {
                                info->io_base = pci_read_bar_io((uint8_t)bus, slot, func, bar_offset);
                                info->mmio_base = 0;
                            } else {
                                info->mmio_base = pci_read_bar64((uint8_t)bus, slot, func, bar_offset);
                                info->io_base = 0;
                            }
                            pci_enable_controller((uint8_t)bus, slot, func, info->bar_is_io);
                        }
                        usb_probe_mmio_caps(info);
#if !ALOS_HW_SAFE
                        usb_probe_xhci_handoff(info);
                        usb_probe_ehci_handoff(info);
#endif
                    }
                }
            }
        }
    }
}

void usb_probe_take_ownership(void) {
    for (int i = 0; i < g_usb_hc_count; ++i) {
        usb_probe_xhci_handoff(&g_usb_hc[i]);
        usb_probe_ehci_handoff(&g_usb_hc[i]);
    }
}

int usb_probe_count(void) {
    return g_usb_hc_count;
}

const UsbHostControllerInfo *usb_probe_get(int index) {
    if (index < 0 || index >= g_usb_hc_count) return 0;
    return &g_usb_hc[index];
}
