#ifndef KERNEL_MEMORY_HEAP_H
#define KERNEL_MEMORY_HEAP_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(ALOS_FORCE_SMALL_HEAP) && ALOS_FORCE_SMALL_HEAP
#define HEAP_SIZE  (8 * 1024 * 1024)   /* mode hardware-safe: heap minimal */
#elif defined(ALOS_HAS_NDS_INTERNAL_CORE) && ALOS_HAS_NDS_INTERNAL_CORE
#define HEAP_SIZE  (256 * 1024 * 1024) /* 256 MiB pour melonDS + ROMs DS */
#else
#define HEAP_SIZE  (8 * 1024 * 1024)   /* 8 MiB (mGBA + jeux VM) */
#endif

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

