/* kernel/memory/pmm.c - Gestionnaire de memoire physique (bitmap + next-fit) */
#include "pmm.h"
#include "../lib/string.h"

static uint32_t bitmap[PMM_MAX_PAGES / 32];
static uint32_t total_pages = 0;
static uint32_t first_page  = 0;  /* numero de la premiere page geree */
static uint32_t next_hint   = 0;  /* curseur next-fit */
static uint32_t free_pages_count = 0;

#define BIT_SET(i)   (bitmap[(i)/32] |=  (1u << ((i)%32)))
#define BIT_CLR(i)   (bitmap[(i)/32] &= ~(1u << ((i)%32)))
#define BIT_TEST(i)  (bitmap[(i)/32] &   (1u << ((i)%32)))

void pmm_init(uint32_t kernel_end, uint32_t mem_end) {
    /* Aligner kernel_end sur la prochaine page */
    first_page = (kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t last_page = mem_end / PAGE_SIZE;
    if (last_page > PMM_MAX_PAGES) last_page = PMM_MAX_PAGES;

    kmemset(bitmap, 0, sizeof(bitmap));

    if (last_page <= first_page) {
        total_pages = 0;
        free_pages_count = 0;
        next_hint = first_page;
        return;
    }

    total_pages = last_page - first_page;
    free_pages_count = total_pages;
    next_hint = first_page;
}

uint32_t pmm_alloc(void) {
    if (!total_pages || !free_pages_count) return 0;

    uint32_t start = next_hint;
    uint32_t end   = first_page + total_pages;

    if (start < first_page || start >= end) start = first_page;

    /* Passe 1: [start, end) */
    for (uint32_t i = start; i < end; i++) {
        if (!BIT_TEST(i)) {
            BIT_SET(i);
            free_pages_count--;
            next_hint = (i + 1 < end) ? (i + 1) : first_page;
            return i * PAGE_SIZE;
        }
    }

    /* Passe 2: [first_page, start) */
    for (uint32_t i = first_page; i < start; i++) {
        if (!BIT_TEST(i)) {
            BIT_SET(i);
            free_pages_count--;
            next_hint = i + 1;
            return i * PAGE_SIZE;
        }
    }

    return 0; /* OOM */
}

void pmm_free(uint32_t addr) {
    uint32_t i = addr / PAGE_SIZE;
    if (i < first_page || i >= first_page + total_pages) return;
    if (!BIT_TEST(i)) return; /* double free / deja libre */

    BIT_CLR(i);
    free_pages_count++;
    if (i < next_hint) next_hint = i;
}

uint32_t pmm_total_pages(void) { return total_pages; }
uint32_t pmm_used_pages(void)  { return total_pages - free_pages_count; }
uint32_t pmm_free_pages(void)  { return free_pages_count; }

