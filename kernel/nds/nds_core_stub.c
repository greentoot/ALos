#include "nds_core.h"

#include "../lib/string.h"

int nds_core_available(void) {
    return 0;
}

const char *nds_core_name(void) {
    return "stub";
}

int nds_core_boot_probe(const char *rom_name,
                        const void *rom_data,
                        uint32_t rom_size,
                        uint32_t frames,
                        char *errbuf,
                        uint32_t errbuf_size) {
    (void)rom_name;
    (void)rom_data;
    (void)rom_size;
    (void)frames;
    if (errbuf && errbuf_size) {
        kstrncpy(errbuf,
                 "core interne Nintendo DS non encore porte dans ALOS",
                 errbuf_size - 1);
        errbuf[errbuf_size - 1] = '\0';
    }
    return -1;
}

int nds_core_run_rom(const char *rom_name,
                     const void *rom_data,
                     uint32_t rom_size,
                     char *errbuf,
                     uint32_t errbuf_size) {
    (void)rom_name;
    (void)rom_data;
    (void)rom_size;
    if (errbuf && errbuf_size) {
        kstrncpy(errbuf,
                 "core interne Nintendo DS non encore porte dans ALOS",
                 errbuf_size - 1);
        errbuf[errbuf_size - 1] = '\0';
    }
    return -1;
}
