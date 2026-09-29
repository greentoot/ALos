/* kernel/gdt.c - GDT + TSS minimal, voir gdt.h pour le contexte complet
 * (pourquoi c'etait du code mort avant, contrainte d'ordre avec
 * keyboard_init(), etc.). */
#include "gdt.h"
#include "lib/string.h"

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} GDTEntry;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
} GDTPointer;

/* TSS 32 bits standard x86. On ne s'en sert pas pour un vrai task-switch
 * materiel (le noyau garde son propre ordonnanceur logiciel, voir
 * kernel/process/scheduler.c) -- seuls ss0/esp0 sont exploites : c'est le
 * mecanisme par lequel le CPU sait quelle pile noyau charger quand une
 * interruption/exception/int 0x80 survient alors qu'une tache ring3 est en
 * cours d'execution. */
typedef struct __attribute__((packed)) {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} TSSEntry;

#define GDT_ENTRIES 6

static GDTEntry   gdt[GDT_ENTRIES];
static GDTPointer gdt_ptr;
static TSSEntry   tss;

static void gdt_set(int idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran_hi) {
    gdt[idx].limit_low   = (uint16_t)(limit & 0xFFFFu);
    gdt[idx].base_low    = (uint16_t)(base & 0xFFFFu);
    gdt[idx].base_mid    = (uint8_t)((base >> 16) & 0xFFu);
    gdt[idx].access      = access;
    gdt[idx].granularity = (uint8_t)(((limit >> 16) & 0x0Fu) | (gran_hi & 0xF0u));
    gdt[idx].base_high   = (uint8_t)((base >> 24) & 0xFFu);
}

/* lgdt puis recharge tous les registres de segment. Style deja utilise
 * ailleurs dans le noyau (blocs __asm__ volatile inline plutot que des
 * fichiers .asm separes pour ce genre de petite routine). */
static void gdt_flush(uint32_t ptr_addr) {
    __asm__ volatile (
        "lgdt (%0)\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "ljmp $0x08, $1f\n\t"
        "1:\n\t"
        :
        : "r" (ptr_addr)
        : "eax", "memory"
    );
}

static void tss_flush(void) {
    __asm__ volatile ("ltr %%ax" : : "a" ((uint16_t)GDT_SEL_TSS));
}

void gdt_init(void) {
    kmemset(&tss, 0, sizeof(tss));

    gdt_set(0, 0, 0, 0, 0);                                  /* nul, obligatoire */
    gdt_set(1, 0, 0xFFFFFu, 0x9Au, 0xC0u);                    /* code ring0, plat 4 Go */
    gdt_set(2, 0, 0xFFFFFu, 0x92u, 0xC0u);                    /* donnees ring0, plat 4 Go */
    gdt_set(3, 0, 0xFFFFFu, 0xFAu, 0xC0u);                    /* code ring3, plat 4 Go */
    gdt_set(4, 0, 0xFFFFFu, 0xF2u, 0xC0u);                    /* donnees ring3, plat 4 Go */
    gdt_set(5, (uint32_t)&tss, sizeof(tss) - 1u, 0x89u, 0x00u); /* TSS */

    tss.ss0  = GDT_SEL_KDATA;
    tss.esp0 = 0; /* pose reellement par tss_set_kernel_stack() avant tout lancement ring3 */
    tss.iomap_base = (uint16_t)sizeof(tss);

    gdt_ptr.limit = (uint16_t)(sizeof(gdt) - 1u);
    gdt_ptr.base  = (uint32_t)&gdt;

    gdt_flush((uint32_t)&gdt_ptr);
    tss_flush();
}

void tss_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
}
