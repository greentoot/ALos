#ifndef KERNEL_PROCESS_TASK_H
#define KERNEL_PROCESS_TASK_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TASK_STACK_SIZE  65536
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
    uint8_t    stack[TASK_STACK_SIZE] __attribute__((aligned(16)));
} Task;

/* Initialise la table des tâches et crée la tâche 0 (kernel idle) */
void tasks_init(void);

/* Crée une nouvelle tâche (retourne pid ou -1) */
int  task_create(const char *name, void (*entry)(void));

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

