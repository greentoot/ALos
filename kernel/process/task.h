#ifndef KERNEL_PROCESS_TASK_H
#define KERNEL_PROCESS_TASK_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TASK_STACK_SIZE       65536
#define TASK_USER_STACK_SIZE  16384
#define MAX_TASKS        8
#define TASK_NAME_LEN    16

typedef enum {
    TASK_READY   = 0,
    TASK_RUNNING = 1,
    TASK_BLOCKED = 2,
    TASK_ZOMBIE  = 3,
} TaskState;

typedef struct Task {
    uint32_t   pid;
    char       name[TASK_NAME_LEN];
    TaskState  state;
    uint32_t   esp;             /* registre ESP sauvegardé */
    uint32_t   sleep_until;     /* timer_ms() cible si BLOCKED */
    int        ring3;           /* 1 si tache utilisateur (voir task_create_user) */
    /* Pile "noyau" de la tache : pour une tache ring0 c'est la pile
     * d'execution normale. Pour une tache ring3 elle n'est JAMAIS executee
     * directement -- le CPU l'utilise uniquement via TSS.esp0 quand une
     * interruption/exception/int 0x80 survient pendant que le code ring3
     * tourne (voir kernel/gdt.c, kernel/process/scheduler.c). */
    uint8_t    stack[TASK_STACK_SIZE] __attribute__((aligned(16)));
    /* Pile utilisateur, utilisee uniquement si ring3 == 1. */
    uint8_t    user_stack[TASK_USER_STACK_SIZE] __attribute__((aligned(16)));
} Task;

/* Initialise la table des tâches et crée la tâche 0 (kernel idle) */
void tasks_init(void);

/* Crée une nouvelle tâche ring0 (retourne pid ou -1) */
int  task_create(const char *name, void (*entry)(void));

/* Crée une nouvelle tâche ring3 (mode utilisateur). "entry" est lancé
 * directement via iret (pas d'indirection C) : il doit s'agir d'une
 * adresse valide en ring3 (ex: e_entry d'un ELF chargé par
 * kernel/exec/elf_loader.c). Le programme doit se terminer en appelant
 * "int 0x80" avec eax=1 (SYS_EXIT) -- tomber en fin de fonction (ret) sans
 * appel systeme explicite est un comportement indéfini (pas de crt0/libc). */
int  task_create_user(const char *name, void (*entry)(void));

/* Tue une tâche par pid */
void task_kill(uint32_t pid);

/* Met la tâche courante en sommeil pendant ms millisecondes */
void task_sleep(uint32_t ms);

/* Retourne la tâche courante */
Task *task_current(void);
int   task_current_index(void);
void  task_set_current_index(int index);

/* Retourne la table (pour ps, etc.) */
Task *task_table(void);

#ifdef __cplusplus
}
#endif

#endif

