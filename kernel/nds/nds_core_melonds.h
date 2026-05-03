#ifndef KERNEL_NDS_NDS_CORE_MELONDS_H
#define KERNEL_NDS_NDS_CORE_MELONDS_H

#include <stdint.h>

int nds_core_boot_probe(const char *rom_name,
                        const void *rom_data,
                        uint32_t rom_size,
                        uint32_t frames,
                        char *errbuf,
                        uint32_t errbuf_size);

#endif
