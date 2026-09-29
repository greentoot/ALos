/* kernel/process/task.c */
#include "task.h"
#include "../lib/string.h"
#include "../../driver/timer.h"
#include "../gdt.h"

/* Etiquette dans driver/idt_stubs.asm, juste apres le "call
 * do_context_switch" de irq0_stub (donc juste avant le "popa; iret"
 * partage par toutes les taches). do_context_switch bascule esp puis
 * fait un simple "ret" : ce "ret" saute a l'adresse tout en haut de la
 * pile de la tache elue. Pour une tache DEJA lancee, cette adresse a ete
 * posee la par le "call" precedent (retour normal). Pour une tache toute
 * neuve, setup_stack() doit la poser explicitement, sinon le "ret" saute
 * dans le vide (c'etait le bug : setup_stack ne le faisait pas, donc
 * task_create() plantait des la premiere tache lancee — #UD des le
 * premier changement de contexte vers elle). */
extern void task_resume_point(void);

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

/* Prepare le stack d'une nouvelle tache pour un premier iret propre.
 *
 * Cas ring0 (existant, inchange) : frame iret a 3 mots (eflags, cs, eip),
 * "cs" reste le selecteur code noyau -- popa;iret (voir task_resume_point
 * dans driver/idt_stubs.asm) reste au meme niveau de privilege, le CPU ne
 * touche pas a esp/ss.
 *
 * Cas ring3 (nouveau) : frame iret a 5 mots. Quand "cs" charge par iret a
 * un RPL different du CS courant (3 vs 0 ici), le CPU depile EN PLUS esp et
 * ss depuis la pile -- c'est le mecanisme standard x86 de changement de
 * privilege via iret, aucun code special n'est necessaire dans le stub
 * assembleur pour ca. L'ordre pousse (du plus haut au plus bas en memoire)
 * doit etre : ss, esp, eflags, cs, eip -- exactement l'ordre inverse de ce
 * que iret depile. */
static void setup_stack(Task *t, void (*entry)(void), int ring3) {
    uint32_t *sp = (uint32_t*)(t->stack + TASK_STACK_SIZE);

    if (ring3) {
        uint32_t *usp = (uint32_t*)(t->user_stack + TASK_USER_STACK_SIZE);
        *--sp = (uint32_t)(GDT_SEL_UDATA | 3); /* ss  : pile utilisateur */
        *--sp = (uint32_t)usp;                 /* esp : sommet pile utilisateur */
        *--sp = 0x202;                         /* eflags : IF=1 */
        *--sp = (uint32_t)(GDT_SEL_UCODE | 3); /* cs  : code ring3 */
        *--sp = (uint32_t)entry;               /* eip : point d'entree */
    } else {
        *--sp = 0x202;            /* eflags : IF=1 */
        *--sp = GDT_SEL_KCODE;    /* cs     : selecteur code kernel */
        *--sp = (uint32_t)entry;  /* eip    : point d'entree de la tache */
    }
    /* Faux pusha (8 registres x 4 octets) */
    for (int i = 0; i < 8; i++) *--sp = 0;
    /* Adresse de retour consommee par le "ret" de do_context_switch */
    *--sp = (uint32_t)task_resume_point;
    t->esp = (uint32_t)sp;
    t->ring3 = ring3;
}

static int task_create_internal(const char *name, void (*entry)(void), int ring3) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_ZOMBIE || (tasks[i].pid == 0 && i != 0 && tasks[i].state == 0 && tasks[i].name[0] == 0)) {
            kmemset(&tasks[i], 0, sizeof(Task));
            tasks[i].pid   = next_pid++;
            tasks[i].state = TASK_READY;
            kstrncpy(tasks[i].name, name, TASK_NAME_LEN);
            setup_stack(&tasks[i], entry, ring3);
            return (int)tasks[i].pid;
        }
    }
    return -1;
}

int task_create(const char *name, void (*entry)(void)) {
    return task_create_internal(name, entry, 0);
}

int task_create_user(const char *name, void (*entry)(void)) {
    return task_create_internal(name, entry, 1);
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

