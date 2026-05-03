#include "vm_interp.h"
#include "../lib/string.h"

VMInstr vm_prog[1];
VMFunc  vm_funcs[1];
int16_t vm_ram[HACK_RAM_SIZE];

void vm_init(VMState *vm) {
    if (!vm) return;
    kmemset(vm, 0, sizeof(*vm));
    vm->main_entry = -1;
}

int vm_load_store(VMState *vm, char *errbuf, uint32_t errsize) {
    (void)vm;
    if (errbuf && errsize) {
        kstrncpy(errbuf, "Jack VM indisponible en mode hardware-safe.", errsize - 1u);
        errbuf[errsize - 1u] = '\0';
    }
    return -1;
}

int vm_load(VMState *vm, const char **files, int nfiles, char *errbuf, uint32_t errsize) {
    (void)vm;
    (void)files;
    (void)nfiles;
    if (errbuf && errsize) {
        kstrncpy(errbuf, "Jack VM indisponible en mode hardware-safe.", errsize - 1u);
        errbuf[errsize - 1u] = '\0';
    }
    return -1;
}

void vm_run(VMState *vm) {
    (void)vm;
}

void vm_destroy(VMState *vm) {
    (void)vm;
}
