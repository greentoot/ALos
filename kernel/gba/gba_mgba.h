#ifndef KERNEL_GBA_GBA_MGBA_H
#define KERNEL_GBA_GBA_MGBA_H

#include <stdint.h>

/* Run an in-memory GB/GBC/GBA ROM until ESC is pressed.
 * Returns 0 on normal exit, <0 on error and fills errbuf when provided.
 */
int gba_mgba_run_rom(const char *rom_name,
                     const void *rom_data,
                     uint32_t rom_size,
                     char *errbuf,
                     uint32_t errbuf_size);

/* Report whether a compatible BIOS blob is available for the requested ROM. */
int gba_mgba_has_bios_for_rom(const char *rom_name, uint32_t *out_size);

/* Persistence bridge for GBA SRAM/Flash saves. */
void gba_mgba_save_clear_all(void);
int  gba_mgba_save_count(void);
int  gba_mgba_save_get(int index, const char **name, const void **data, uint32_t *size);
int  gba_mgba_save_set(const char *name, const void *data, uint32_t size);

#endif
