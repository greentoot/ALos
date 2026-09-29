/* kernel/memory/paging.c - Pagination identite (PSE, pages 4 Mo) */
#include "paging.h"
#include "../boot/earlydiag.h"

#define PDE_COUNT 1024

/* Repertoire de pages : 1024 entrees x 4 octets = 4 Ko, doit etre aligne
 * sur 4 Ko (CR3 pointe dessus, les 12 bits bas de CR3 sont des flags). */
static uint32_t page_directory[PDE_COUNT] __attribute__((aligned(4096)));

static int g_paging_enabled = 0;

/* CPUID.1:EDX bit 3 = PSE (Page Size Extension, pages larges 4 Mo). */
static int cpu_has_pse(void) {
    uint32_t edx;
    __asm__ volatile (
        "mov $1, %%eax\n\t"
        "cpuid\n\t"
        : "=d"(edx)
        :
        : "eax", "ebx", "ecx"
    );
    return (edx & (1u << 3)) != 0;
}

void paging_init(void) {
    if (!cpu_has_pse()) {
        earlydiag_stage("paging: PSE absent, pagination desactivee");
        g_paging_enabled = 0;
        return;
    }

    /* Mapping identite complet (0..4 Go) par pages de 4 Mo : PDE[i] couvre
     * l'intervalle physique/virtuel [i*4Mo, (i+1)*4Mo). present(0) |
     * writable(1) | PS=page 4Mo(7). Pas de bit NX gere ici : le micro-
     * assembleur (kernel/exec/asm_exec.c) execute du code JIT depuis des
     * buffers .bss/.data, ce qui doit continuer a fonctionner a
     * l'identique une fois la pagination active. */
    for (uint32_t i = 0; i < PDE_COUNT; ++i) {
        page_directory[i] = (i << 22) | 0x83u;
    }

    __asm__ volatile (
        /* CR4.PSE (bit 4) : autorise les pages larges de 4 Mo. */
        "mov %%cr4, %%eax\n\t"
        "or  $0x10, %%eax\n\t"
        "mov %%eax, %%cr4\n\t"
        /* CR3 = adresse physique du repertoire. Le noyau tourne sans
         * pagination avant cet appel donc adresse virtuelle courante ==
         * adresse physique : page_directory est deja un pointeur valide
         * a donner tel quel a CR3. */
        "mov %0, %%eax\n\t"
        "mov %%eax, %%cr3\n\t"
        /* CR0.PG (bit 31) : active la pagination. Identite -> l'eip
         * courant reste valide immediatement apres, pas de saut requis. */
        "mov %%cr0, %%eax\n\t"
        "or  $0x80000000, %%eax\n\t"
        "mov %%eax, %%cr0\n\t"
        :
        : "r" (page_directory)
        : "eax", "memory"
    );

    g_paging_enabled = 1;
    earlydiag_stage("paging: pagination identite active (PSE 4Mo)");
}

int paging_is_enabled(void) {
    return g_paging_enabled;
}
