#ifndef DRIVER_ATA_H
#define DRIVER_ATA_H

#include <stdint.h>

#define ATA_MAX_DEVICES 4

typedef struct {
    uint8_t  present;
    uint8_t  atapi;
    uint8_t  channel; /* 0=primary, 1=secondary */
    uint8_t  drive;   /* 0=master, 1=slave */
    uint32_t sectors;
    char     model[41];
} AtaDeviceInfo;

void ata_init(void);
int  ata_is_present(void);
int  ata_device_count(void);
const AtaDeviceInfo *ata_get_device(int index);

/* Reads ATA IDENTIFY words (256 words) for primary master. */
int  ata_identify(uint16_t *out256);
int  ata_identify_device(int index, uint16_t *out256);

/* PIO LBA28 sector IO (512 bytes each). */
int  ata_read_sector(uint32_t lba, uint8_t *buf512);
int  ata_write_sector(uint32_t lba, const uint8_t *buf512);
int  ata_read_sectors(uint32_t lba, uint32_t count, void *buf);
int  ata_write_sectors(uint32_t lba, uint32_t count, const void *buf);
int  ata_read_sector_device(int index, uint32_t lba, uint8_t *buf512);
int  ata_write_sector_device(int index, uint32_t lba, const uint8_t *buf512);
int  ata_read_sectors_device(int index, uint32_t lba, uint32_t count, void *buf);
int  ata_write_sectors_device(int index, uint32_t lba, uint32_t count, const void *buf);

#endif
