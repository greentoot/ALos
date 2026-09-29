#ifndef DRIVER_RTL8139_H
#define DRIVER_RTL8139_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pilote RTL8139 minimal (PCI, I/O-port, mode "polling" -- pas d'IRQ, voir
 * kernel/net/net.c pour le contexte complet et les limites assumees de la
 * premiere tranche de pile reseau). QEMU emule ce chipset nativement, donc
 * pas besoin de pilote plus complexe pour tester : lancer QEMU avec
 * "-device rtl8139,netdev=n0 -netdev user,id=n0" (voir le README). */

int  rtl8139_init(void);          /* scanne le bus PCI, initialise la carte si trouvee. 1=ok, 0=absente/echec */
int  rtl8139_present(void);
const uint8_t *rtl8139_mac(void); /* pointeur vers les 6 octets d'adresse MAC */

/* Emet une trame Ethernet complete (en-tete inclus, PAS le FCS -- la carte
 * l'ajoute). Retourne 1 si accepte pour transmission, 0 sinon. */
int  rtl8139_send(const void *frame, uint32_t len);

/* Recupere UNE trame en attente (sans bloquer). Retourne la taille copiee
 * (0 si aucune trame disponible), -1 si erreur. buf doit faire au moins
 * 1518 octets (MTU Ethernet standard + en-tetes). */
int  rtl8139_poll_recv(void *buf, uint32_t bufsize);

#ifdef __cplusplus
}
#endif

#endif
