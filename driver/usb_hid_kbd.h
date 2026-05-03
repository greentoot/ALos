#ifndef DRIVER_USB_HID_KBD_H
#define DRIVER_USB_HID_KBD_H

/*
 * driver/usb_hid_kbd.h — ALOS USB HID Boot Keyboard driver
 *
 * Strategie : xHCI/OHCI polling, Boot Protocol, sans stack USB complete.
 * Un seul clavier USB à la fois (premier HID boot keyboard trouvé).
 *
 * Dépendances :
 *   driver/usb_probe.h   — UsbHostControllerInfo, usb_probe_get()
 *   driver/usb_xhci.h    — usb_xhci_present(), usb_xhci_connected_ports()
 *   driver/keyboard.h    — keyboard_inject_tap(), keyboard_set_external_modifiers()
 *   kernel/memory/pmm.h  — pmm_alloc() pour les ring TRB (4K-alignés)
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * usb_hid_kbd_init()
 *   À appeler après usb_probe_init() + usb_xhci_init().
 *   Tente d'enumerer le premier clavier HID sur xHCI, puis OHCI.
 *   Retourne 1 si un clavier a été trouvé et initialisé, 0 sinon.
 */
int usb_hid_kbd_init(void);

/*
 * usb_hid_kbd_poll()
 *   À appeler dans la boucle principale du shell (ou timer).
 *   Lit le rapport HID courant et injecte les touches dans keyboard.c.
 */
void usb_hid_kbd_poll(void);

/* Diagnostic */
int  usb_hid_kbd_present(void);
void usb_hid_kbd_status_string(char *buf, uint32_t bufsz);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_USB_HID_KBD_H */
