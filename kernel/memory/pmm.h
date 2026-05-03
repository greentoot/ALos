#ifndef KERNEL_MEMORY_PMM_H
#define KERNEL_MEMORY_PMM_H
#include <stdint.h>
#include <stdbool.h>

#define PAGE_SIZE   4096
#define PMM_MAX_PAGES 262144   /* 1 Gio / 4 Ko */

/* Initialise le bitmap ?? partir de kernel_end jusqu'?? mem_end */
void pmm_init(uint32_t kernel_end, uint32_t mem_end);

/* Alloue une frame physique (retourne adresse ou 0 si OOM) */
uint32_t pmm_alloc(void);

/* Lib??re une frame */
void pmm_free(uint32_t addr);

/* Stats */
uint32_t pmm_free_pages(void);
uint32_t pmm_total_pages(void);
uint32_t pmm_used_pages(void);

#endif

