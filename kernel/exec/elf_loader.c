/* kernel/exec/elf_loader.c - Chargeur ELF32 (i386, ET_EXEC) minimal.
 * Voir elf_loader.h pour la note importante sur le modele de confiance
 * (ring0, pas d'isolation memoire -- meme esprit que asm_exec.c). */
#include "elf_loader.h"
#include "../process/task.h"
#include "../lib/string.h"

typedef struct __attribute__((packed)) {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

#define PT_LOAD 1u
#define ET_EXEC 2u
#define EM_386  3u

/* Bornes de securite : refuse un segment qui ecraserait le bas de la
 * memoire (noyau charge a 1 Mo, tas juste au-dessus) ou qui deborderait
 * au-dela d'une limite raisonnable. Ce n'est PAS une vraie isolation
 * memoire (voir elf_loader.h) -- juste un garde-fou contre les erreurs
 * les plus grossieres (mauvais linker script, fichier corrompu). */
#define ELF_MIN_VADDR 0x00200000u /* 2 Mo : marge sous le noyau/tas */
#define ELF_MAX_VADDR 0x10000000u /* 256 Mo */

static int elf_err(char *errbuf, uint32_t errsize, const char *msg, int code) {
    if (errbuf && errsize) {
        kstrncpy(errbuf, msg, errsize - 1);
        errbuf[errsize - 1] = '\0';
    }
    return code;
}

/* Meme mecanisme que pending_image[] dans asm_exec.c : on depose le
 * point d'entree dans le slot qu'on s'apprete a donner a task_create(),
 * la tache neuve le recupere au premier lancement. */
static void *pending_entry[MAX_TASKS];

static void elf_task_entry(void) {
    Task *me = task_current();
    Task *table = task_table();
    int slot = -1;
    void (*fn)(void) = 0;

    for (int i = 0; i < MAX_TASKS; i++) {
        if (&table[i] == me) { slot = i; break; }
    }
    if (slot >= 0 && pending_entry[slot]) {
        fn = (void (*)(void))pending_entry[slot];
        pending_entry[slot] = 0;
        fn();
    }
    if (slot >= 0) table[slot].state = TASK_ZOMBIE;
    __asm__ volatile ("cli");
    while (1) __asm__ volatile ("hlt");
}

int elf_loader_run(const void *data, uint32_t size, const char *task_name,
                    int ring3, char *errbuf, uint32_t errsize) {
    const uint8_t *bytes = (const uint8_t *)data;
    const Elf32_Ehdr *eh;
    const Elf32_Phdr *ph;

    if (!data || size < sizeof(Elf32_Ehdr))
        return elf_err(errbuf, errsize, "elf: fichier trop court", ELF_ERR_TOOSHORT);

    eh = (const Elf32_Ehdr *)bytes;

    if (eh->e_ident[0] != 0x7Fu || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return elf_err(errbuf, errsize, "elf: signature invalide (pas un fichier ELF)", ELF_ERR_BADMAGIC);
    if (eh->e_ident[4] != 1u) /* ELFCLASS32 */
        return elf_err(errbuf, errsize, "elf: seul ELF32 est supporte", ELF_ERR_BADCLASS);
    if (eh->e_ident[5] != 1u) /* ELFDATA2LSB */
        return elf_err(errbuf, errsize, "elf: seul le little-endian est supporte", ELF_ERR_BADCLASS);
    if (eh->e_machine != EM_386)
        return elf_err(errbuf, errsize, "elf: architecture non i386", ELF_ERR_BADARCH);
    if (eh->e_type != ET_EXEC)
        return elf_err(errbuf, errsize, "elf: seul ET_EXEC (statique, non-PIE) est supporte", ELF_ERR_BADTYPE);
    if (eh->e_phoff == 0 || eh->e_phnum == 0)
        return elf_err(errbuf, errsize, "elf: pas de program headers", ELF_ERR_TOOSHORT);
    if ((uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf32_Phdr) > (uint64_t)size)
        return elf_err(errbuf, errsize, "elf: table de program headers hors du fichier", ELF_ERR_TOOSHORT);

    ph = (const Elf32_Phdr *)(bytes + eh->e_phoff);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr *p = &ph[i];
        uint8_t *dst;

        if (p->p_type != PT_LOAD) continue;
        if (p->p_vaddr < ELF_MIN_VADDR || p->p_vaddr > ELF_MAX_VADDR)
            return elf_err(errbuf, errsize, "elf: adresse de segment hors des bornes autorisees", ELF_ERR_BADADDR);
        if ((uint64_t)p->p_vaddr + (uint64_t)p->p_memsz > (uint64_t)ELF_MAX_VADDR)
            return elf_err(errbuf, errsize, "elf: segment trop grand / deborde des bornes", ELF_ERR_BADADDR);
        if ((uint64_t)p->p_offset + (uint64_t)p->p_filesz > (uint64_t)size)
            return elf_err(errbuf, errsize, "elf: segment hors du fichier", ELF_ERR_TOOSHORT);

        dst = (uint8_t *)p->p_vaddr;
        if (p->p_filesz) kmemcpy(dst, bytes + p->p_offset, p->p_filesz);
        if (p->p_memsz > p->p_filesz) kmemset(dst + p->p_filesz, 0, p->p_memsz - p->p_filesz);
    }

    if (eh->e_entry < ELF_MIN_VADDR || eh->e_entry > ELF_MAX_VADDR)
        return elf_err(errbuf, errsize, "elf: point d'entree hors des bornes autorisees", ELF_ERR_BADADDR);

    if (ring3) {
        /* Pas d'indirection C ici (contrairement au chemin ring0
         * ci-dessous) : on ne peut pas "appeler" du code ring3 depuis du
         * C ring0, seul iret peut faire cette transition. e_entry est
         * directement embarque comme EIP de la fausse frame iret par
         * task_create_user() (voir kernel/process/task.c, setup_stack). */
        int pid = task_create_user(task_name, (void (*)(void))eh->e_entry);
        if (pid < 0)
            return elf_err(errbuf, errsize, "elf: creation de tache (ring3) echouee", ELF_ERR_TASK);
        return ELF_OK;
    }

    {
        Task *table = task_table();
        int free_slot = -1;
        int pid;

        for (int i = 0; i < MAX_TASKS; i++) {
            if (table[i].name[0] == 0 || table[i].state == TASK_ZOMBIE) { free_slot = i; break; }
        }
        if (free_slot < 0)
            return elf_err(errbuf, errsize, "elf: table de taches pleine", ELF_ERR_TASK);

        pending_entry[free_slot] = (void *)eh->e_entry;

        pid = task_create(task_name, elf_task_entry);
        if (pid < 0) {
            pending_entry[free_slot] = 0;
            return elf_err(errbuf, errsize, "elf: creation de tache echouee", ELF_ERR_TASK);
        }
    }

    return ELF_OK;
}
