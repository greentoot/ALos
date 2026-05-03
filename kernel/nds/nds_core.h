#ifndef KERNEL_NDS_NDS_CORE_H
#define KERNEL_NDS_NDS_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int nds_core_available(void);
const char *nds_core_name(void);
int nds_core_boot_probe(const char *rom_name,
                        const void *rom_data,
                        uint32_t rom_size,
                        uint32_t frames,
                        char *errbuf,
                        uint32_t errbuf_size);
int nds_core_run_rom(const char *rom_name,
                     const void *rom_data,
                     uint32_t rom_size,
                     char *errbuf,
                     uint32_t errbuf_size);

#ifdef __cplusplus
}
#endif

#endif
