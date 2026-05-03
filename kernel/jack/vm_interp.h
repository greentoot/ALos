#ifndef KERNEL_JACK_VM_INTERP_H
#define KERNEL_JACK_VM_INTERP_H
#include <stdint.h>

/*
 * Hack VM Interpreter — Nand2Tetris compatible
 *
 * RAM Hack :
 *   [0]       SP        (initial 256)
 *   [1]       LCL
 *   [2]       ARG
 *   [3]       THIS
 *   [4]       THAT
 *   [5..12]   temp
 *   [13..15]  R13-R15
 *   [256..2047]   stack
 *   [2048..16383] heap
 *   [16384..24575] screen (512×256 px 1-bit)
 *   [24576]   clavier
 *   [28672..32767] static (zone dédiée, évite collision stack/heap)
 */

#define HACK_RAM_SIZE    32768
#define HACK_SCREEN_BASE 16384
#define HACK_KBD_ADDR    24576
#define HACK_STACK_BASE  256
#define HACK_HEAP_BASE   2048
#define HACK_STATIC_BASE 28672

/* Limites programme (dimensionnees pour jeux Jack volumineux) */
#define VM_MAX_INSTRS   100000
#define VM_MAX_FUNCS    512
#define VM_MAX_NAME     96
#define VM_MAX_LABELS   16384
#define VM_MAX_PATCHES  16384
#define VM_MAX_OS_CALLS 8192   /* appels OS enregistrés */

typedef enum {
    OP_PUSH_CONST=0, OP_PUSH_LOCAL, OP_PUSH_ARG,
    OP_PUSH_THIS, OP_PUSH_THAT, OP_PUSH_TEMP,
    OP_PUSH_POINTER, OP_PUSH_STATIC,
    OP_POP_LOCAL, OP_POP_ARG, OP_POP_THIS, OP_POP_THAT,
    OP_POP_TEMP, OP_POP_POINTER, OP_POP_STATIC,
    OP_ADD, OP_SUB, OP_NEG, OP_EQ, OP_GT, OP_LT,
    OP_AND, OP_OR, OP_NOT,
    OP_LABEL, OP_GOTO, OP_IF_GOTO,
    OP_FUNCTION, OP_CALL, OP_RETURN,
} VMOpcode;

/* VMInstr compacté : 9 bytes au lieu de 16 → économise ~400 KB pour Pokemon */
typedef struct __attribute__((packed)) {
    uint8_t  op;          /* opcode (30 valeurs max → uint8_t) */
    int16_t  arg;         /* nlocals / nargs / index segment    */
    int32_t  target;      /* PC cible (goto/call) ou -1=OS      */
    uint16_t static_id;   /* index statique absolu              */
} VMInstr;

typedef struct {
    char     name[VM_MAX_NAME];
    int32_t  entry;
} VMFunc;

/* ── État de la VM ─────────────────────────────────────────────────────── */
/* prog[] et ram[] sont des tableaux STATIQUES globaux (section .bss)
   pour éviter une allocation heap de 250+ KB.
   VMState ne contient que les scalaires + pointeurs. */
typedef struct {
    int32_t  prog_len;
    int32_t  nfuncs;
    int32_t  pc;
    int32_t  main_entry;  /* index de Main.main (-1 si absent) */
    int      running;
    int      screen_dirty;
} VMState;

/* Tableaux statiques globaux (définis dans vm_interp.c) */
extern VMInstr  vm_prog[];
extern VMFunc   vm_funcs[];
extern int16_t  vm_ram[];

/* ── API ────────────────────────────────────────────────────────────────── */
void vm_init (VMState *vm);
/* Charge tous les .vm depuis vm_store directement (sans lookup par nom) */
int  vm_load_store(VMState *vm, char *errbuf, uint32_t errsize);

int  vm_load (VMState *vm, const char **files, int nfiles,
              char *errbuf, uint32_t errsize);
void vm_run  (VMState *vm);
void vm_destroy(VMState *vm);   /* libère les ressources dynamiques éventuelles */

#endif
