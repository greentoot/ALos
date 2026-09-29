#ifndef KERNEL_MEMORY_PAGING_H
#define KERNEL_MEMORY_PAGING_H

#include <stdint.h>

/* Pagination x86 minimale : mapping "identite" (virtuel == physique) sur
 * la totalite de l'espace 4 Go, via des pages larges de 4 Mo (PSE). Pas
 * de tables de pages a allouer : un seul repertoire de 1024 entrees
 * suffit. Premiere brique vers une gestion memoire type Linux (cf.
 * complet_list : "Pagination memoire") ; pas encore de separation
 * noyau/utilisateur (tout reste ring0, cote pas de bit NX) -- ca viendra
 * avec le futur mode utilisateur (ring3).
 *
 * Si le CPU ne supporte pas PSE (cas tres rare), la pagination reste
 * desactivee et le noyau continue de fonctionner exactement comme avant
 * (adressage physique direct, sans MMU active). */
void paging_init(void);

/* Vrai si la pagination a effectivement ete activee (CR0.PG=1). */
int paging_is_enabled(void);

#endif
