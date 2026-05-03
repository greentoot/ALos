#include "usb_xhci.h"

#include "usb_probe.h"
#include "../kernel/lib/kprintf.h"
#include "../kernel/lib/string.h"

#define XHCI_PORTSC_BASE   0x400u
#define XHCI_PORTSC_STRIDE 0x10u

#define XHCI_USBCMD  0x00u
#define XHCI_USBSTS  0x04u

#define XHCI_STS_HCH (1u << 0)
#define XHCI_STS_CNR (1u << 11)

#define XHCI_PORTSC_CCS        (1u << 0)
#define XHCI_PORTSC_PORT_SPEED(v) (((v) >> 10) & 0x0Fu)

typedef struct {
    int present;
    int ready;
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint8_t port_count;
    uint8_t slot_count;
    uint8_t connected_count;
    uint32_t mmio_base;
    uint32_t op_base;
    uint32_t connected_bitmap;
    uint8_t port_speed[16];
} UsbXhciState;

static UsbXhciState g_xhci;

static uint32_t mmio_read32(uint32_t addr) {
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static void xhci_refresh_ports(void) {
    uint8_t max_ports;

    g_xhci.connected_bitmap = 0;
    g_xhci.connected_count = 0;
    kmemset(g_xhci.port_speed, 0, sizeof(g_xhci.port_speed));

    max_ports = g_xhci.port_count;
    if (max_ports > 16) max_ports = 16;

    for (uint8_t port = 0; port < max_ports; ++port) {
        uint32_t portsc = mmio_read32(g_xhci.op_base + XHCI_PORTSC_BASE + ((uint32_t)port * XHCI_PORTSC_STRIDE));
        if (portsc & XHCI_PORTSC_CCS) {
            g_xhci.connected_bitmap |= (1u << port);
            g_xhci.port_speed[port] = (uint8_t)XHCI_PORTSC_PORT_SPEED(portsc);
            g_xhci.connected_count++;
        }
    }
}

void usb_xhci_init(void) {
    const UsbHostControllerInfo *info = 0;
    int count = usb_probe_count();

    kmemset(&g_xhci, 0, sizeof(g_xhci));

    for (int i = 0; i < count; ++i) {
        const UsbHostControllerInfo *cur = usb_probe_get(i);
        if (cur && cur->kind == USB_HC_XHCI && cur->mmio_base && cur->mmio_base <= 0xFFFFFFFFull) {
            info = cur;
            break;
        }
    }

    if (!info) return;

    g_xhci.present = 1;
    g_xhci.bus = info->bus;
    g_xhci.slot = info->slot;
    g_xhci.func = info->func;
    g_xhci.port_count = info->port_count;
    g_xhci.slot_count = info->slot_count;
    g_xhci.mmio_base = (uint32_t)info->mmio_base;
    g_xhci.op_base = g_xhci.mmio_base + (uint32_t)info->cap_length;
    xhci_refresh_ports();

    {
        uint32_t usbsts = mmio_read32(g_xhci.op_base + XHCI_USBSTS);
        g_xhci.ready = ((usbsts & XHCI_STS_CNR) == 0) ? 1 : 0;
        if (usbsts & XHCI_STS_HCH) {
            /* Halted est acceptable pour notre premier socle: le MMIO reste exploitable. */
            g_xhci.ready = 1;
        }
    }
}

void usb_xhci_poll(void) {
    if (!g_xhci.present) return;
    xhci_refresh_ports();
}

int usb_xhci_present(void) {
    return g_xhci.present;
}

int usb_xhci_ready(void) {
    return g_xhci.ready;
}

uint32_t usb_xhci_connected_ports(void) {
    return g_xhci.connected_bitmap;
}

void usb_xhci_status_string(char *buf, uint32_t bufsz) {
    char tmp[96];

    if (!buf || bufsz == 0) return;
    buf[0] = '\0';

    if (!g_xhci.present) {
        kstrncpy(buf, "xHCI absent", bufsz - 1);
        buf[bufsz - 1] = '\0';
        return;
    }

    ksprintf(tmp,
             "xHCI %s mmio=%x ports=%u slots=%u conn=%u mask=%x",
             g_xhci.ready ? "pret" : "non-pret",
             (unsigned)g_xhci.mmio_base,
             (unsigned)g_xhci.port_count,
             (unsigned)g_xhci.slot_count,
             (unsigned)g_xhci.connected_count,
             (unsigned)g_xhci.connected_bitmap);
    kstrncpy(buf, tmp, bufsz - 1);
    buf[bufsz - 1] = '\0';
}
