#ifndef KERNEL_FS_PERSIST_H
#define KERNEL_FS_PERSIST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent snapshot service:
 * - mutable RamFS tree
 * - GBA save slots
 */

void persist_init(void);
void persist_mark_dirty(void);
void persist_flush(void);
int  persist_is_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
