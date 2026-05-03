/* kernel/process/task.c */
#include "task.h"
#include "../lib/string.h"
#include "../../driver/timer.h"

static Task  tasks[MAX_TASKS];
static int   current_index = 0;
static uint32_t next_pid = 0;

Task *task_table(void)   { return tasks; }
Task *task_current(void) { return &tasks[current_index]; }
int   task_current_index(void) { return current_index; }
void  task_set_current_index(int index) {
    if (index < 0) index = 0;
    if (index >= MAX_TASKS) index = MAX_TASKS - 1;
    current_index = index;
}

void tasks_init(void) {
    kmemset(tasks, 0, sizeof(tasks));
    /* Tache 0 = kernel shell (deja en cours d'execution) */
    tasks[0].pid   = next_pid++;
    tasks[0].state = TASK_RUNNING;
    kstrncpy(tasks[0].name, "kshell", TASK_NAME_LEN);
    current_index = 0;
}

/* Prepare le stack d'une nouvelle tache pour un premier iret propre */
static void setup_stack(Task *t, void (*entry)(void)) {
    uint32_t *sp = (uint32_t*)(t->stack + TASK_STACK_SIZE);
    /* Faux frame iret : eflags, cs, eip */
    *--sp = 0x202;            /* eflags : IF=1 */
    *--sp = 0x08;             /* cs     : selecteur code kernel */
    *--sp = (uint32_t)entry;  /* eip    : point d'entree de la tache */
    /* Faux pusha (8 registres x 4 octets) */
    for (int i = 0; i < 8; i++) *--sp = 0;
    t->esp = (uint32_t)sp;
}

int task_create(const char *name, void (*entry)(void)) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_ZOMBIE || (tasks[i].pid == 0 && i != 0 && tasks[i].state == 0 && tasks[i].name[0] == 0)) {
            kmemset(&tasks[i], 0, sizeof(Task));
            tasks[i].pid   = next_pid++;
            tasks[i].state = TASK_READY;
            kstrncpy(tasks[i].name, name, TASK_NAME_LEN);
            setup_stack(&tasks[i], entry);
            return (int)tasks[i].pid;
        }
    }
    return -1;
}

void task_kill(uint32_t pid) {
    if (pid == 0) return; /* ne pas tuer kshell */
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].pid == pid && tasks[i].state != TASK_ZOMBIE)
            tasks[i].state = TASK_ZOMBIE;
}

void task_sleep(uint32_t ms) {
    task_current()->state       = TASK_BLOCKED;
    task_current()->sleep_until = timer_ms() + ms;
    /* Yield cooperatif: rester bloque jusqu'au reveil explicite du scheduler. */
    while (task_current()->state == TASK_BLOCKED)
        __asm__ volatile ("hlt");
}

