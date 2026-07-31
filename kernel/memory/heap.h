#ifndef KERNEL_MEMORY_HEAP_H
#define KERNEL_MEMORY_HEAP_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HEAP_SIZE  (8 * 1024 * 1024)

void  heap_init(uint32_t heap_start);
void *kmalloc(uint32_t size);
void  kfree(void *ptr);
uint32_t ksize(const void *ptr);

/* Stats */
uint32_t heap_used(void);
uint32_t heap_free(void);
uint32_t heap_largest_free(void);
uint32_t heap_block_count(void);
uint32_t heap_free_block_count(void);

#ifdef __cplusplus
}
#endif

#endif

