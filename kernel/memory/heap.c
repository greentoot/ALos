/* kernel/memory/heap.c - Allocateur kernel (next-fit + coalescence locale) */
#include "heap.h"
#include "../lib/string.h"

typedef struct Block {
    uint32_t      size;   /* taille utile (sans header) */
    uint8_t       free;
    struct Block *next;
} Block;

#define HEADER_SIZE ((uint32_t)sizeof(Block))
#define HEAP_ALIGN  8u

static Block   *head  = 0;
static Block   *rover = 0;
static uint8_t  heap_area[HEAP_SIZE];

static uint32_t align_up(uint32_t v) {
    return (v + (HEAP_ALIGN - 1u)) & ~(HEAP_ALIGN - 1u);
}

static int ptr_in_heap(const void *p) {
    const uint8_t *b = (const uint8_t*)p;
    return b >= heap_area && b < (heap_area + HEAP_SIZE);
}

static int blocks_are_adjacent(Block *a, Block *b) {
    uint8_t *end_a = (uint8_t*)a + HEADER_SIZE + a->size;
    return end_a == (uint8_t*)b;
}

static void coalesce_forward(Block *b) {
    while (b && b->next && b->free && b->next->free && blocks_are_adjacent(b, b->next)) {
        b->size += HEADER_SIZE + b->next->size;
        b->next = b->next->next;
    }
}

void heap_init(uint32_t heap_start) {
    (void)heap_start; /* zone statique, simple et deterministic */
    head = (Block*)heap_area;
    head->size = HEAP_SIZE - HEADER_SIZE;
    head->free = 1;
    head->next = 0;
    rover = head;
}

void *kmalloc(uint32_t size) {
    if (!size) return 0;
    if (!head) heap_init(0);

    size = align_up(size);

    Block *start = rover ? rover : head;
    Block *cur = start;

    do {
        if (cur->free && cur->size >= size) {
            /* Split si le reste peut contenir un bloc utile */
            if (cur->size >= size + HEADER_SIZE + HEAP_ALIGN) {
                Block *newb = (Block*)((uint8_t*)cur + HEADER_SIZE + size);
                newb->size = cur->size - size - HEADER_SIZE;
                newb->free = 1;
                newb->next = cur->next;
                cur->size  = size;
                cur->next  = newb;
            }
            cur->free = 0;
            rover = cur->next ? cur->next : head;
            return (void*)((uint8_t*)cur + HEADER_SIZE);
        }

        cur = cur->next ? cur->next : head;
    } while (cur && cur != start);

    return 0; /* OOM */
}

void kfree(void *ptr) {
    if (!ptr || !head) return;

    uint8_t *raw = (uint8_t*)ptr;
    if (raw < heap_area + HEADER_SIZE || raw >= heap_area + HEAP_SIZE) return;

    Block *b = (Block*)(raw - HEADER_SIZE);
    if (!ptr_in_heap(b)) return;
    if (b->free) return; /* double free */

    b->free = 1;

    /* Coalescence avant */
    coalesce_forward(b);

    /* Coalescence arriere (chercher le precedent) */
    Block *prev = 0;
    for (Block *cur = head; cur && cur != b; cur = cur->next) prev = cur;

    if (prev && prev->free && blocks_are_adjacent(prev, b)) {
        prev->size += HEADER_SIZE + b->size;
        prev->next = b->next;
        coalesce_forward(prev);
        rover = prev;
    } else {
        rover = b;
    }
}

uint32_t ksize(const void *ptr) {
    const uint8_t *raw;
    const Block *b;
    if (!ptr || !head) return 0;

    raw = (const uint8_t*)ptr;
    if (raw < heap_area + HEADER_SIZE || raw >= heap_area + HEAP_SIZE) return 0;

    b = (const Block*)(raw - HEADER_SIZE);
    if (!ptr_in_heap(b) || b->free) return 0;
    return b->size;
}

uint32_t heap_used(void) {
    uint32_t used = 0;
    for (Block *cur = head; cur; cur = cur->next) {
        if (!cur->free) used += cur->size + HEADER_SIZE;
    }
    return used;
}

uint32_t heap_free(void) {
    uint32_t freeb = 0;
    for (Block *cur = head; cur; cur = cur->next) {
        if (cur->free) freeb += cur->size;
    }
    return freeb;
}

uint32_t heap_largest_free(void) {
    uint32_t best = 0;
    for (Block *cur = head; cur; cur = cur->next) {
        if (cur->free && cur->size > best) best = cur->size;
    }
    return best;
}

uint32_t heap_block_count(void) {
    uint32_t n = 0;
    for (Block *cur = head; cur; cur = cur->next) n++;
    return n;
}

uint32_t heap_free_block_count(void) {
    uint32_t n = 0;
    for (Block *cur = head; cur; cur = cur->next) {
        if (cur->free) n++;
    }
    return n;
}

