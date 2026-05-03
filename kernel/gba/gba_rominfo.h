#ifndef KERNEL_GBA_GBA_ROMINFO_H
#define KERNEL_GBA_GBA_ROMINFO_H

#include <stdint.h>

typedef struct {
    uint8_t valid;
    char header_title[13];
    char game_code[5];
    char maker_code[3];
    uint8_t version;
    uint32_t rom_size;
    char save_desc[16];
    char hw_desc[16];
    uint8_t cover_id;
} GbaRomInfo;

void gba_rom_info_init(GbaRomInfo *info);
int gba_rom_info_parse(const void *rom_data, uint32_t rom_size, GbaRomInfo *info);

#endif
