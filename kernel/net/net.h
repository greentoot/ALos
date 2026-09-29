#ifndef KERNEL_NET_NET_H
#define KERNEL_NET_NET_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pile reseau minimale : pilote RTL8139 (driver/rtl8139.c, polling) +
 * Ethernet + ARP + ICMP echo. PAS de TCP/UDP/sockets, PAS de DHCP -- IP
 * fixe codee en dur (voir net.c, NET_LOCAL_IP), PAS d'IRQ (tout est en
 * polling). C'est une premiere tranche volontairement reduite par rapport
 * a "une pile reseau" au sens complet ; voir le README pour le detail de
 * ce qui manque et pourquoi. Suffisant pour repondre a un ping ICMP venant
 * du reseau (ou en emettre un via `net ping`, voir kernel/shell_linux_
 * like.c), rien de plus. */

void net_init(void);
int  net_available(void);
const uint8_t *net_local_ip(void);   /* 4 octets */
const uint8_t *net_local_mac(void);  /* 6 octets */

/* A appeler periodiquement (ex: boucle du shell) pour traiter le trafic
 * entrant : repond automatiquement aux requetes ARP "who-has <notre IP>"
 * et aux requetes ICMP echo ("ping") qui nous sont adressees. */
void net_poll(void);

/* Resout puis ping une IPv4 (notation "a.b.c.d" deja parsee en 4 octets).
 * Bloquant (boucle de polling avec timeout), pensé pour un usage shell
 * interactif. Retourne le round-trip en ms si succes, -1 si timeout/echec.
 * cache d'ARP volontairement limite a UNE seule entree (la derniere IP
 * resolue) -- voir net.c. */
int net_ping(const uint8_t ip[4], uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
