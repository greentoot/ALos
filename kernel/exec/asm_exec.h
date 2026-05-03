#ifndef KERNEL_EXEC_ASM_EXEC_H
#define KERNEL_EXEC_ASM_EXEC_H
#include <stdint.h>

#define ASMEXEC_OK          0
#define ASMEXEC_ERR_SYNTAX  1   /* instruction / opérande inconnue     */
#define ASMEXEC_ERR_UNDEF   2   /* label indéfini                      */
#define ASMEXEC_ERR_OOM     3   /* mémoire insuffisante                */
#define ASMEXEC_ERR_TOOLONG 4   /* programme trop grand                */

/*
 * Assemble src (texte ASM i386 simplifié) et crée une tâche nommée name.
 * Instructions supportées : mov add sub inc dec xor and or
 *                           cmp jmp je jne jl jg jle jge
 *                           call ret push pop int nop hlt
 * Commentaires : ; jusqu'à fin de ligne
 * Registres    : eax ebx ecx edx esi edi esp ebp
 * Syscall INT 0x80 : eax=numéro, ebx/ecx/edx=args (same as kernel syscall)
 *
 * Retourne ASMEXEC_OK ou un code d'erreur.
 * Si errbuf != NULL, le message d'erreur y est copié.
 */
int asmexec_run(const char *name, const char *src,
                char *errbuf, uint32_t errsize);

#endif
