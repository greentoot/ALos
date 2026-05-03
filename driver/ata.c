#include "ata.h"

#define ATA_PRIMARY_IO     0x1F0
#define ATA_PRIMARY_CTRL   0x3F6
#define ATA_SECONDARY_IO   0x170
#define ATA_SECONDARY_CTRL 0x376

#define ATA_REG_DATA       0
#define ATA_REG_ERROR      1
#define ATA_REG_FEATURES   1
#define ATA_REG_SECCNT     2
#define ATA_REG_LBA0       3
#define ATA_REG_LBA1       4
#define ATA_REG_LBA2       5
#define ATA_REG_HDDEVSEL   6
#define ATA_REG_STATUS     7
#define ATA_REG_COMMAND    7

#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_READ_SECT   0x20
#define ATA_CMD_WRITE_SECT  0x30
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_SR_ERR   0x01
#define ATA_SR_DRQ   0x08
#define ATA_SR_DF    0x20
#define ATA_SR_DRDY  0x40
#define ATA_SR_BSY   0x80

typedef struct {
    uint16_t io;
    uint16_t ctrl;
} AtaChannel;

static const AtaChannel g_channels[2] = {
    { ATA_PRIMARY_IO, ATA_PRIMARY_CTRL },
    { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL },
};

static AtaDeviceInfo g_ata_devices[ATA_MAX_DEVICES];
static int g_ata_count = 0;
static int g_ata_default = -1;

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile ("inw %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0,%1" :: "a"(v), "Nd"(port));
}

static void ata_delay_400ns(uint16_t ctrl_port) {
    (void)inb(ctrl_port);
    (void)inb(ctrl_port);
    (void)inb(ctrl_port);
    (void)inb(ctrl_port);
}

static int ata_wait_not_bsy(uint16_t io_base) {
    for (uint32_t i = 0; i < 2000000u; i++) {
        uint8_t s = inb((uint16_t)(io_base + ATA_REG_STATUS));
        if ((s & ATA_SR_BSY) == 0) return 0;
    }
    return -1;
}

static int ata_wait_drq(uint16_t io_base) {
    for (uint32_t i = 0; i < 2000000u; i++) {
        uint8_t s = inb((uint16_t)(io_base + ATA_REG_STATUS));
        if (s & ATA_SR_ERR) return -1;
        if (s & ATA_SR_DF) return -1;
        if ((s & ATA_SR_BSY) == 0 && (s & ATA_SR_DRQ)) return 0;
    }
    return -1;
}

static void ata_select_device(const AtaChannel *ch, uint8_t drive, uint32_t lba_high4) {
    outb((uint16_t)(ch->io + ATA_REG_HDDEVSEL),
         (uint8_t)(0xE0 | ((drive & 1u) << 4) | (lba_high4 & 0x0Fu)));
    ata_delay_400ns(ch->ctrl);
}

static void ata_extract_model(char *out, const uint16_t *id) {
    int p = 0;
    if (!out || !id) return;
    for (int i = 27; i <= 46; i++) {
        char hi = (char)(id[i] >> 8);
        char lo = (char)(id[i] & 0xFF);
        if (p < 40) out[p++] = hi;
        if (p < 40) out[p++] = lo;
    }
    while (p > 0 && out[p - 1] == ' ') p--;
    out[p] = '\0';
}

static int ata_identify_channel_drive(const AtaChannel *ch,
                                      uint8_t channel,
                                      uint8_t drive,
                                      uint16_t *out256,
                                      AtaDeviceInfo *out_info) {
    uint8_t status;
    uint8_t sig1, sig2;

    ata_select_device(ch, drive, 0);
    outb((uint16_t)(ch->io + ATA_REG_SECCNT), 0);
    outb((uint16_t)(ch->io + ATA_REG_LBA0), 0);
    outb((uint16_t)(ch->io + ATA_REG_LBA1), 0);
    outb((uint16_t)(ch->io + ATA_REG_LBA2), 0);
    outb((uint16_t)(ch->io + ATA_REG_COMMAND), ATA_CMD_IDENTIFY);
    ata_delay_400ns(ch->ctrl);

    status = inb((uint16_t)(ch->io + ATA_REG_STATUS));
    if (status == 0x00 || status == 0xFF) return -1;
    if (ata_wait_not_bsy(ch->io) != 0) return -1;

    sig1 = inb((uint16_t)(ch->io + ATA_REG_LBA1));
    sig2 = inb((uint16_t)(ch->io + ATA_REG_LBA2));
    if (sig1 != 0 || sig2 != 0) {
        if (out_info) {
            out_info->present = 1;
            out_info->atapi = 1;
            out_info->channel = channel;
            out_info->drive = drive;
            out_info->sectors = 0;
            out_info->model[0] = '\0';
        }
        return -2;
    }

    if (ata_wait_drq(ch->io) != 0) return -1;

    for (int i = 0; i < 256; i++) out256[i] = inw((uint16_t)(ch->io + ATA_REG_DATA));

    if (out_info) {
        out_info->present = 1;
        out_info->atapi = 0;
        out_info->channel = channel;
        out_info->drive = drive;
        out_info->sectors = ((uint32_t)out256[61] << 16) | out256[60];
        ata_extract_model(out_info->model, out256);
    }
    return 0;
}

static const AtaDeviceInfo *ata_info_or_null(int index) {
    if (index < 0 || index >= ATA_MAX_DEVICES) return 0;
    if (!g_ata_devices[index].present) return 0;
    return &g_ata_devices[index];
}

static int ata_rw_device(int index, uint32_t lba, uint32_t count, void *buf, int is_write) {
    const AtaDeviceInfo *dev = ata_info_or_null(index);
    const AtaChannel *ch;
    uint8_t *p = (uint8_t*)buf;
    if (!dev || dev->atapi || !buf) return -1;
    if (count == 0) return 0;
    if ((lba + count) < lba) return -1;
    if (dev->sectors && (lba + count > dev->sectors)) return -1;
    ch = &g_channels[dev->channel];

    for (uint32_t i = 0; i < count; i++) {
        uint32_t cur_lba = lba + i;
        if (ata_wait_not_bsy(ch->io) != 0) return -1;
        ata_select_device(ch, dev->drive, (cur_lba >> 24) & 0x0F);
        outb((uint16_t)(ch->io + ATA_REG_FEATURES), 0);
        outb((uint16_t)(ch->io + ATA_REG_SECCNT), 1);
        outb((uint16_t)(ch->io + ATA_REG_LBA0), (uint8_t)(cur_lba & 0xFF));
        outb((uint16_t)(ch->io + ATA_REG_LBA1), (uint8_t)((cur_lba >> 8) & 0xFF));
        outb((uint16_t)(ch->io + ATA_REG_LBA2), (uint8_t)((cur_lba >> 16) & 0xFF));
        outb((uint16_t)(ch->io + ATA_REG_COMMAND), is_write ? ATA_CMD_WRITE_SECT : ATA_CMD_READ_SECT);

        if (ata_wait_drq(ch->io) != 0) return -1;

        if (is_write) {
            for (int w = 0; w < 256; w++) {
                uint16_t word = (uint16_t)p[i * 512u + w * 2 + 0] |
                                ((uint16_t)p[i * 512u + w * 2 + 1] << 8);
                outw((uint16_t)(ch->io + ATA_REG_DATA), word);
            }
            outb((uint16_t)(ch->io + ATA_REG_COMMAND), ATA_CMD_CACHE_FLUSH);
            if (ata_wait_not_bsy(ch->io) != 0) return -1;
        } else {
            for (int w = 0; w < 256; w++) {
                uint16_t word = inw((uint16_t)(ch->io + ATA_REG_DATA));
                p[i * 512u + w * 2 + 0] = (uint8_t)(word & 0xFF);
                p[i * 512u + w * 2 + 1] = (uint8_t)(word >> 8);
            }
        }
    }
    return 0;
}

void ata_init(void) {
    uint16_t identify[256];
    g_ata_count = 0;
    g_ata_default = -1;
    for (int i = 0; i < ATA_MAX_DEVICES; i++) {
        g_ata_devices[i].present = 0;
        g_ata_devices[i].atapi = 0;
        g_ata_devices[i].channel = (uint8_t)(i / 2);
        g_ata_devices[i].drive = (uint8_t)(i & 1);
        g_ata_devices[i].sectors = 0;
        g_ata_devices[i].model[0] = '\0';
    }

    for (uint8_t channel = 0; channel < 2; channel++) {
        const AtaChannel *ch = &g_channels[channel];
        for (uint8_t drive = 0; drive < 2; drive++) {
            int idx = channel * 2 + drive;
            int rc = ata_identify_channel_drive(ch, channel, drive, identify, &g_ata_devices[idx]);
            if (rc == 0) {
                g_ata_count++;
                if (g_ata_default < 0) g_ata_default = idx;
            } else if (rc == -2) {
                g_ata_devices[idx].present = 1;
                g_ata_devices[idx].atapi = 1;
            }
        }
    }
}

int ata_is_present(void) {
    return g_ata_default >= 0;
}

int ata_device_count(void) {
    return g_ata_count;
}

const AtaDeviceInfo *ata_get_device(int index) {
    return ata_info_or_null(index);
}

int ata_identify(uint16_t *out256) {
    return ata_identify_device(g_ata_default, out256);
}

int ata_identify_device(int index, uint16_t *out256) {
    const AtaDeviceInfo *dev = ata_info_or_null(index);
    if (!dev || dev->atapi || !out256) return -1;
    return ata_identify_channel_drive(&g_channels[dev->channel],
                                      dev->channel,
                                      dev->drive,
                                      out256,
                                      0);
}

int ata_read_sector(uint32_t lba, uint8_t *buf512) {
    return ata_read_sector_device(g_ata_default, lba, buf512);
}

int ata_write_sector(uint32_t lba, const uint8_t *buf512) {
    return ata_write_sector_device(g_ata_default, lba, buf512);
}

int ata_read_sectors(uint32_t lba, uint32_t count, void *buf) {
    return ata_read_sectors_device(g_ata_default, lba, count, buf);
}

int ata_write_sectors(uint32_t lba, uint32_t count, const void *buf) {
    return ata_write_sectors_device(g_ata_default, lba, count, (void*)buf);
}

int ata_read_sector_device(int index, uint32_t lba, uint8_t *buf512) {
    return ata_rw_device(index, lba, 1, buf512, 0);
}

int ata_write_sector_device(int index, uint32_t lba, const uint8_t *buf512) {
    return ata_rw_device(index, lba, 1, (void*)buf512, 1);
}

int ata_read_sectors_device(int index, uint32_t lba, uint32_t count, void *buf) {
    return ata_rw_device(index, lba, count, buf, 0);
}

int ata_write_sectors_device(int index, uint32_t lba, uint32_t count, const void *buf) {
    return ata_rw_device(index, lba, count, (void*)buf, 1);
}
