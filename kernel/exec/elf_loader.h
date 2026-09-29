#ifndef KERNEL_EXEC_ELF_LOADER_H
#define KERNEL_EXEC_ELF_LOADER_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Chargeur ELF32 (i386, ET_EXEC) minimal.
 *
 * Comme le micro-assembleur (kernel/exec/asm_exec.c), le code charge
 * tourne en ring 0, SANS isolation memoire : il n'y a pas encore de mode
 * utilisateur (ring3) dans ALOS. C'est donc un chargeur "de confiance" --
 * il ne protege pas le noyau contre un ELF malveillant ou bugge, tout
 * comme asm_exec ne protege pas contre un programme assembleur qui ecrit
 * n'importe ou. A reevaluer quand le mode utilisateur arrivera.
 *
 * Les segments PT_LOAD sont copies tels quels a leur p_vaddr (mapping
 * identite grace a kernel/memory/paging.c : virt == phys), le bss est
 * mis a zero. Le point d'entree (e_entry) est lance comme une nouvelle
 * tache via task_create(), donc preemptible comme n'importe quel autre
 * programme (asm ou natif).
 *
 * Convention d'adresse attendue pour les ELF de test : linker a partir
 * de 0x00200000 (2 Mo), voir le README pour la recette de compilation.
 *
 * Depuis cette session, un second mode existe : ring3 == 1 lance e_entry
 * directement en mode utilisateur (voir kernel/gdt.c et kernel/process/
 * task.c, task_create_user()) au lieu de ring0. Un programme lance en
 * ring3 DOIT se terminer en appelant "int 0x80" avec eax=1 (SYS_EXIT) --
 * il n'y a pas de crt0/libc, tomber en fin de fonction sans appel systeme
 * explicite est un comportement indefini (la pile utilisateur ne contient
 * aucune adresse de retour valide). */

#define ELF_OK             0
#define ELF_ERR_BADMAGIC  -1
#define ELF_ERR_BADCLASS  -2
#define ELF_ERR_BADARCH   -3
#define ELF_ERR_BADTYPE   -4
#define ELF_ERR_BADADDR   -5
#define ELF_ERR_TOOSHORT  -6
#define ELF_ERR_TASK      -7

/* Charge et lance un ELF32 deja entierement present en memoire (ex:
 * module multiboot, ou fichier lu depuis ramfs/diskfs). "ring3" = 0 pour
 * un lancement noyau (comportement historique), 1 pour un lancement en
 * mode utilisateur. Retourne ELF_OK ou un code d'erreur negatif ; errbuf
 * recoit un message lisible. */
int elf_loader_run(const void *data, uint32_t size, const char *task_name,
                    int ring3, char *errbuf, uint32_t errsize);

#ifdef __cplusplus
}
#endif

#endif
