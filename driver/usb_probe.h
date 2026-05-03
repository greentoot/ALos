#ifndef DRIVER_USB_PROBE_H
#define DRIVER_USB_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    USB_HC_NONE = 0,
    USB_HC_UHCI = 1,
    USB_HC_OHCI = 2,
    USB_HC_EHCI = 3,
    USB_HC_XHCI = 4,
    USB_HC_OTHER = 5
};

typedef struct {
    uint8_t present;
    uint8_t kind;
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t vendor;
    uint16_t device;
    uint8_t prog_if;
    uint8_t irq_line;
    uint8_t cap_length;
    uint16_t hci_version;
    uint8_t port_count;
    uint8_t slot_count;
    uint64_t mmio_base;
    uint16_t io_base;
    uint8_t bar_is_io;
    uint16_t legacy_cap_offset;
    uint8_t legacy_bios_owned;
    uint8_t legacy_os_owned;
    uint8_t legacy_handoff_ok;
} UsbHostControllerInfo;

void usb_probe_init(void);
void usb_probe_take_ownership(void);
int usb_probe_count(void);
const UsbHostControllerInfo *usb_probe_get(int index);
const char *usb_probe_kind_name(uint8_t kind);

#ifdef __cplusplus
}
#endif

#endif
