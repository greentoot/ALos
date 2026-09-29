/* kernel/process/scheduler.c - Round-robin preemptif robuste */
#include "scheduler.h"
#include "task.h"
#include "../../driver/timer.h"
#include "../lib/string.h"
#include "../gdt.h"

/* Pose esp0 dans le TSS pour la tache qui va etre elue : c'est la pile que
 * le CPU chargera automatiquement si CETTE tache est interrompue alors
 * qu'elle tourne en ring3 (IRQ, exception, int 0x80). Sans ca, une
 * interruption pendant l'execution d'une tache ring3 utiliserait une pile
 * noyau perimee (celle de la derniere tache pour laquelle esp0 avait ete
 * pose) -- corruption memoire quasi garantie. Inoffensif pour les taches
 * ring0 (elles n'utilisent jamais esp0). */
static void arm_kernel_stack(Task *tasks, int idx) {
    tss_set_kernel_stack((uint32_t)(tasks[idx].stack + TASK_STACK_SIZE));
}

static int cur_idx = 0;

void scheduler_init(void) { cur_idx = 0; }

uint32_t scheduler_tick(uint32_t current_esp) {
    Task *tasks = task_table();
    if (cur_idx < 0 || cur_idx >= MAX_TASKS) cur_idx = 0;
    task_set_current_index(cur_idx);

    /* Reveiller les taches dont le timer est ecoule */
    uint32_t now = timer_ms();
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_BLOCKED && now >= tasks[i].sleep_until)
            tasks[i].state = TASK_READY;
    }

    /* Sauvegarder l'ESP de la tache courante */
    tasks[cur_idx].esp = current_esp;
    if (tasks[cur_idx].state == TASK_RUNNING)
        tasks[cur_idx].state = TASK_READY;

    /* Chercher la prochaine tache prete (round-robin) */
    for (int i = 1; i <= MAX_TASKS; i++) {
        int idx = (cur_idx + i) % MAX_TASKS;
        if (tasks[idx].state == TASK_READY && tasks[idx].name[0] != 0) {
            cur_idx = idx;
            tasks[cur_idx].state = TASK_RUNNING;
            task_set_current_index(cur_idx);
            arm_kernel_stack(tasks, cur_idx);
            return tasks[cur_idx].esp;
        }
    }

    /* Fallback 1: relancer la courante si elle est runnable */
    if (tasks[cur_idx].name[0] != 0 &&
        (tasks[cur_idx].state == TASK_READY || tasks[cur_idx].state == TASK_RUNNING)) {
        tasks[cur_idx].state = TASK_RUNNING;
        task_set_current_index(cur_idx);
        arm_kernel_stack(tasks, cur_idx);
        return tasks[cur_idx].esp;
    }

    /* Fallback 2: trouver n'importe quelle tache runnable */
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].name[0] == 0) continue;
        if (tasks[i].state == TASK_READY || tasks[i].state == TASK_RUNNING) {
            cur_idx = i;
            tasks[cur_idx].state = TASK_RUNNING;
            task_set_current_index(cur_idx);
            arm_kernel_stack(tasks, cur_idx);
            return tasks[cur_idx].esp;
        }
    }

    /* Ultime fallback: aucune tache runnable */
    task_set_current_index(cur_idx);
    arm_kernel_stack(tasks, cur_idx);
    return current_esp;
}

