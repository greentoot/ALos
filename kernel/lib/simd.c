/* kernel/lib/simd.c - Detection + activation SSE2 pour l'acceleration de
 * kmemcpy/kmemset (kernel/lib/string.c).
 *
 * IMPORTANT : ce fichier n'est PAS compile avec -msse2 dans les CFLAGS
 * globales (voir Makefile) -- volontairement, pour eviter que l'auto-
 * vectorisation de gcc -O3 ne glisse des instructions SSE dans du code
 * execute AVANT que CR0/CR4 soient configures ici (ce qui declencherait un
 * #UD des le tout premier memset/memcpy du boot). Seules les fonctions
 * explicitement marquees __attribute__((target("sse2"))) (ici et dans
 * string.c) generent du code SSE2 ; le reste du noyau reste strictement
 * scalaire au niveau codegen, quel que soit le niveau d'optimisation. */
#include "simd.h"
#include <stdint.h>

static int g_simd_available = 0;

static int cpu_has_sse2(void) {
    uint32_t edx;
    __asm__ volatile ("cpuid" : "=d" (edx) : "a" (1) : "ebx", "ecx");
    return (edx >> 26) & 1u;
}

void simd_init(void) {
    if (!cpu_has_sse2()) {
        g_simd_available = 0;
        return;
    }

    /* CR0 : EM (bit 2) = 0 -> pas d'emulation FPU/SIMD par le CPU (les
     * instructions SSE ne declenchent pas #NM) ; MP (bit 1) = 1 -> WAIT/FWAIT
     * respecte TS. CR4 : OSFXSR (bit 9) = 1 -> le systeme d'exploitation
     * gere FXSAVE/FXRSTOR, condition necessaire pour que le CPU autorise
     * les instructions SSE (sinon #UD) ; OSXMMEXCPT (bit 10) = 1 -> les
     * exceptions flottantes SIMD non masquees remontent en #XF plutot
     * qu'en #UD. Ce noyau ne fait pas de veritable sauvegarde FXSAVE/
     * FXRSTOR par tache (pas de FPU/SIMD state par tache pour l'instant) --
     * limite acceptable tant que seul kmemcpy/kmemset l'utilisent en
     * "scratch" pur (xmm0 seulement, jamais suppose survivre a un
     * changement de contexte). */
    __asm__ volatile (
        "mov %%cr0, %%eax\n\t"
        "and $0xFFFFFFFBu, %%eax\n\t"
        "or  $0x2u, %%eax\n\t"
        "mov %%eax, %%cr0\n\t"
        "mov %%cr4, %%eax\n\t"
        "or  $0x600u, %%eax\n\t"
        "mov %%eax, %%cr4\n\t"
        :
        :
        : "eax", "memory"
    );

    g_simd_available = 1;
}

int simd_available(void) { return g_simd_available; }
