#ifndef KERNEL_LIB_SIMD_H
#define KERNEL_LIB_SIMD_H

/* Detection + activation SSE2 (CPUID puis CR0/CR4). A appeler une fois, tot
 * dans kernel_main(), avant que des copies/remplissages volumineux ne
 * profitent de l'acceleration (voir kernel/lib/string.c, kmemcpy/kmemset).
 *
 * Pas de contrainte stricte d'ordre : simd_available() vaut 0 tant que
 * simd_init() n'a pas tourne (ou si le CPU ne supporte pas SSE2), donc tout
 * appel a kmemcpy/kmemset AVANT simd_init() reste silencieusement sur son
 * chemin scalaire existant -- aucun risque de #UD pour du code appele tot
 * au boot. */
void simd_init(void);
int  simd_available(void);

#endif
