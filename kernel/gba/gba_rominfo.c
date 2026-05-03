#include "gba_rominfo.h"

#include "../lib/string.h"

static int gba_match3(const char code[5], char a, char b, char c) {
    return code[0] == a && code[1] == b && code[2] == c;
}

static int gba_blob_contains(const uint8_t *data, uint32_t size, const char *needle) {
    uint32_t nlen;
    if (!data || !needle) return 0;
    nlen = kstrlen(needle);
    if (nlen == 0 || size < nlen) return 0;
    for (uint32_t i = 0; i + nlen <= size; i++) {
        if (kmemcmp(data + i, needle, nlen) == 0) return 1;
    }
    return 0;
}

static void gba_copy_ascii_field(char *dst, uint32_t dstsz, const uint8_t *src, uint32_t count) {
    uint32_t n = 0;
    if (!dst || dstsz == 0) return;
    while (n + 1 < dstsz && n < count) {
        char c = (char)src[n];
        if (c == '\0' || c == ' ') break;
        if ((unsigned char)c < 32 || (unsigned char)c > 126) break;
        dst[n] = c;
        n++;
    }
    dst[n] = '\0';
}

void gba_rom_info_init(GbaRomInfo *info) {
    if (!info) return;
    kmemset(info, 0, sizeof(*info));
    kstrcpy(info->save_desc, "AUTODETECT");
    kstrcpy(info->hw_desc, "NONE");
}

int gba_rom_info_parse(const void *rom_data, uint32_t rom_size, GbaRomInfo *info) {
    const uint8_t *rom = (const uint8_t*)rom_data;
    if (!rom_data || rom_size < 0xC0 || !info) return 0;

    gba_rom_info_init(info);
    info->valid = 1;
    info->rom_size = rom_size;

    gba_copy_ascii_field(info->header_title, sizeof(info->header_title), rom + 0xA0, 12);
    gba_copy_ascii_field(info->game_code, sizeof(info->game_code), rom + 0xAC, 4);
    gba_copy_ascii_field(info->maker_code, sizeof(info->maker_code), rom + 0xB0, 2);
    info->version = rom[0xBC];

    if (gba_blob_contains(rom, rom_size, "FLASH1M_V")) {
        kstrcpy(info->save_desc, "FLASH1M");
    } else if (gba_blob_contains(rom, rom_size, "FLASH512_V") ||
               gba_blob_contains(rom, rom_size, "FLASH_V")) {
        kstrcpy(info->save_desc, "FLASH");
    } else if (gba_blob_contains(rom, rom_size, "EEPROM_V")) {
        kstrcpy(info->save_desc, "EEPROM");
    } else if (gba_blob_contains(rom, rom_size, "SRAM_V")) {
        kstrcpy(info->save_desc, "SRAM");
    }

    if (gba_blob_contains(rom, rom_size, "SIIRTC_V") ||
        gba_blob_contains(rom, rom_size, "RTC_V")) {
        kstrcpy(info->hw_desc, "RTC");
    }

    if (gba_match3(info->game_code, 'B', 'P', 'E')) {
        kstrcpy(info->save_desc, "FLASH1M");
        kstrcpy(info->hw_desc, "RTC");
        info->cover_id = 2;
    } else if (gba_match3(info->game_code, 'B', 'P', 'R') ||
               gba_match3(info->game_code, 'B', 'P', 'G')) {
        kstrcpy(info->save_desc, "FLASH1M");
        kstrcpy(info->hw_desc, "NONE");
        info->cover_id = 1;
    }

    return 1;
}
