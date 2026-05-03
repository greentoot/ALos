#ifndef DRIVER_USB_XHCI_H
#define DRIVER_USB_XHCI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void usb_xhci_init(void);
void usb_xhci_poll(void);
int usb_xhci_present(void);
int usb_xhci_ready(void);
uint32_t usb_xhci_connected_ports(void);
void usb_xhci_status_string(char *buf, uint32_t bufsz);

#ifdef __cplusplus
}
#endif

#endif
