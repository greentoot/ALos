#ifndef KERNEL_PROCESS_SCHEDULER_H
#define KERNEL_PROCESS_SCHEDULER_H
#include <stdint.h>

/* Initialise le scheduler (appeler après tasks_init) */
void scheduler_init(void);

/* Appelé par irq0_stub : reçoit l'ESP courant, retourne le nouvel ESP.
   Gère aussi le réveil des tâches endormies. */
uint32_t scheduler_tick(uint32_t current_esp);

#endif
