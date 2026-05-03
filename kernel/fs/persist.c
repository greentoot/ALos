#include "persist.h"

#include "ramfs.h"
#include "../gba/gba_mgba.h"
#include "../memory/heap.h"
#include "../lib/string.h"
#include "../lib/kprintf.h"
#include "../../driver/ata.h"

#include <stdint.h>

#define PERSIST_PART_TYPE 0xA0
#define PERSIST_MAGIC "ALOSPST1"
#define PERSIST_VERSION 1u

#define ENTRY_RAMFS_DIR  1u
#define ENTRY_RAMFS_FILE 2u
#define ENTRY_GBA_SAVE   3u

typedef struct __attribute__((packed)) {
    char magic[8];
    uint32_t version;
    uint32_t total_size;
    uint32_t crc32;
} PersistHeader;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint16_t name_len;
    uint32_t size;
} PersistEntryHeader;

static uint8_t g_enabled = 0;
static uint8_t g_dirty = 0;
static uint8_t g_loading = 0;
static uint8_t g_flushing = 0;
static uint32_t g_part_lba = 0;
static uint32_t g_part_sectors = 0;

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t crc32_calc(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            uint32_t mask = (uint32_t)-(int)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static int persist_find_partition(void) {
    uint8_t mbr[512];
    if (!ata_is_present()) return -1;
    if (ata_read_sector(0, mbr) != 0) return -1;
    if (!(mbr[510] == 0x55 && mbr[511] == 0xAA)) return -1;

    for (int i = 0; i < 4; i++) {
        const uint8_t *e = &mbr[446 + i * 16];
        uint8_t type = e[4];
        uint32_t start = rd32(&e[8]);
        uint32_t count = rd32(&e[12]);
        if (type == PERSIST_PART_TYPE && start && count >= 128) {
            g_part_lba = start;
            g_part_sectors = count;
            return 0;
        }
    }
    return -1;
}

static uint32_t persist_compute_size(void) {
    uint32_t total = (uint32_t)sizeof(PersistHeader);
    int cap = ramfs_node_capacity();

    for (int i = 0; i < cap; i++) {
        const RamFSNode *n = ramfs_node_at(i);
        uint16_t name_len;
        if (!n || !n->used) continue;
        if (n->type == RAMFS_NODE_RO) continue;
        if (kstrcmp(n->name, "/") == 0) continue;
        name_len = (uint16_t)kstrlen(n->name);
        if (n->type == RAMFS_NODE_DIR) {
            total += (uint32_t)sizeof(PersistEntryHeader) + name_len;
        } else if (n->type == RAMFS_NODE_FILE) {
            total += (uint32_t)sizeof(PersistEntryHeader) + name_len + n->size;
        }
    }

    {
        int nsave = gba_mgba_save_count();
        for (int i = 0; i < nsave; i++) {
            const char *name = 0;
            const void *data = 0;
            uint32_t sz = 0;
            if (gba_mgba_save_get(i, &name, &data, &sz) != 0) continue;
            if (!name || !data || sz == 0) continue;
            total += (uint32_t)sizeof(PersistEntryHeader) + (uint16_t)kstrlen(name) + sz;
        }
    }

    return total;
}

static int persist_write_snapshot(void) {
    uint32_t total = persist_compute_size();
    uint32_t sectors = (total + 511u) / 512u;
    uint32_t bytes = sectors * 512u;
    uint8_t *buf;
    uint32_t off;

    if (sectors == 0 || sectors > g_part_sectors) return -1;
    buf = (uint8_t*)kmalloc(bytes);
    if (!buf) return -1;
    kmemset(buf, 0, bytes);

    off = (uint32_t)sizeof(PersistHeader);

    {
        int cap = ramfs_node_capacity();
        for (int i = 0; i < cap; i++) {
            const RamFSNode *n = ramfs_node_at(i);
            PersistEntryHeader eh;
            uint16_t name_len;
            if (!n || !n->used) continue;
            if (n->type == RAMFS_NODE_RO) continue;
            if (kstrcmp(n->name, "/") == 0) continue;
            name_len = (uint16_t)kstrlen(n->name);
            if (n->type == RAMFS_NODE_DIR) {
                eh.type = ENTRY_RAMFS_DIR;
                eh.name_len = name_len;
                eh.size = 0;
            } else if (n->type == RAMFS_NODE_FILE) {
                eh.type = ENTRY_RAMFS_FILE;
                eh.name_len = name_len;
                eh.size = n->size;
            } else {
                continue;
            }

            kmemset(buf + off, 0, sizeof(PersistEntryHeader));
            buf[off + 0] = eh.type;
            wr16(buf + off + 1, eh.name_len);
            wr32(buf + off + 3, eh.size);
            off += (uint32_t)sizeof(PersistEntryHeader);

            kmemcpy(buf + off, n->name, name_len);
            off += name_len;

            if (eh.type == ENTRY_RAMFS_FILE && eh.size) {
                kmemcpy(buf + off, ramfs_data((RamFSNode*)n), eh.size);
                off += eh.size;
            }
        }
    }

    {
        int nsave = gba_mgba_save_count();
        for (int i = 0; i < nsave; i++) {
            const char *name = 0;
            const void *data = 0;
            uint32_t sz = 0;
            uint16_t name_len;
            if (gba_mgba_save_get(i, &name, &data, &sz) != 0) continue;
            if (!name || !data || sz == 0) continue;
            name_len = (uint16_t)kstrlen(name);

            buf[off + 0] = ENTRY_GBA_SAVE;
            wr16(buf + off + 1, name_len);
            wr32(buf + off + 3, sz);
            off += (uint32_t)sizeof(PersistEntryHeader);

            kmemcpy(buf + off, name, name_len);
            off += name_len;
            kmemcpy(buf + off, data, sz);
            off += sz;
        }
    }

    if (off != total) {
        kfree(buf);
        return -1;
    }

    {
        PersistHeader *h = (PersistHeader*)buf;
        kmemcpy(h->magic, PERSIST_MAGIC, 8);
        h->version = PERSIST_VERSION;
        h->total_size = total;
        h->crc32 = crc32_calc(buf + sizeof(PersistHeader), total - (uint32_t)sizeof(PersistHeader));
    }

    if (ata_write_sectors(g_part_lba, sectors, buf) != 0) {
        kfree(buf);
        return -1;
    }

    kfree(buf);
    return 0;
}

static void persist_apply_snapshot(const uint8_t *buf, uint32_t total) {
    g_loading = 1;
    ramfs_clear_mutable();
    gba_mgba_save_clear_all();

    /* Pass 1..N: create directories first (parents before children). */
    for (int iter = 0; iter < RAMFS_MAX_NODES; iter++) {
        int changed = 0;
        uint32_t off = (uint32_t)sizeof(PersistHeader);
        while (off + sizeof(PersistEntryHeader) <= total) {
            PersistEntryHeader eh;
            char path[RAMFS_MAX_PATH];
            uint32_t name_len;

            eh.type = buf[off + 0];
            eh.name_len = (uint16_t)(buf[off + 1] | ((uint16_t)buf[off + 2] << 8));
            eh.size = rd32(buf + off + 3);
            off += (uint32_t)sizeof(PersistEntryHeader);
            name_len = eh.name_len;

            if (name_len == 0 || name_len >= RAMFS_MAX_PATH || off + name_len > total) { off = total; break; }
            kmemcpy(path, buf + off, name_len);
            path[name_len] = '\0';
            off += name_len;

            if (eh.type == ENTRY_RAMFS_DIR) {
                if (!ramfs_find(path) && ramfs_mkdir(path) == RAMFS_OK) changed = 1;
            } else if (eh.type == ENTRY_RAMFS_FILE || eh.type == ENTRY_GBA_SAVE) {
                if (off + eh.size > total) { off = total; break; }
                off += eh.size;
            } else {
                off = total;
                break;
            }
        }
        if (!changed) break;
    }

    /* Pass final: files + GBA saves. */
    {
        uint32_t off = (uint32_t)sizeof(PersistHeader);
        while (off + sizeof(PersistEntryHeader) <= total) {
            PersistEntryHeader eh;
            char path[RAMFS_MAX_PATH];
            uint32_t name_len;

            eh.type = buf[off + 0];
            eh.name_len = (uint16_t)(buf[off + 1] | ((uint16_t)buf[off + 2] << 8));
            eh.size = rd32(buf + off + 3);
            off += (uint32_t)sizeof(PersistEntryHeader);
            name_len = eh.name_len;

            if (name_len == 0 || name_len >= RAMFS_MAX_PATH || off + name_len > total) break;
            kmemcpy(path, buf + off, name_len);
            path[name_len] = '\0';
            off += name_len;

            if (eh.type == ENTRY_RAMFS_FILE) {
                if (off + eh.size > total) break;
                ramfs_create_bytes(path, buf + off, eh.size);
                off += eh.size;
            } else if (eh.type == ENTRY_GBA_SAVE) {
                if (off + eh.size > total) break;
                gba_mgba_save_set(path, buf + off, eh.size);
                off += eh.size;
            } else if (eh.type == ENTRY_RAMFS_DIR) {
                /* already handled */
            } else {
                break;
            }
        }
    }

    g_loading = 0;
}

int persist_is_enabled(void) {
    return g_enabled;
}

void persist_init(void) {
    uint8_t sec[512];
    PersistHeader h;
    uint32_t sectors;
    uint32_t bytes;
    uint8_t *buf;

    g_enabled = 0;
    g_dirty = 0;
    g_loading = 0;
    g_flushing = 0;

    ata_init();
    if (persist_find_partition() != 0) return;

    if (ata_read_sector(g_part_lba, sec) != 0) return;
    kmemcpy(&h, sec, sizeof(h));

    /* Blank partition: enable and wait for first write. */
    if (kmemcmp(h.magic, PERSIST_MAGIC, 8) != 0) {
        int blank = 1;
        for (int i = 0; i < 512; i++) {
            if (sec[i] != 0) { blank = 0; break; }
        }
        if (blank) {
            g_enabled = 1;
            return;
        }
        return;
    }

    if (h.version != PERSIST_VERSION) return;
    if (h.total_size < sizeof(PersistHeader)) return;
    sectors = (h.total_size + 511u) / 512u;
    if (sectors == 0 || sectors > g_part_sectors) return;
    bytes = sectors * 512u;

    buf = (uint8_t*)kmalloc(bytes);
    if (!buf) return;
    if (ata_read_sectors(g_part_lba, sectors, buf) != 0) {
        kfree(buf);
        return;
    }

    if (crc32_calc(buf + sizeof(PersistHeader), h.total_size - (uint32_t)sizeof(PersistHeader)) != h.crc32) {
        kfree(buf);
        return;
    }

    persist_apply_snapshot(buf, h.total_size);
    kfree(buf);
    g_enabled = 1;
}

void persist_flush(void) {
    if (!g_enabled || !g_dirty || g_loading || g_flushing) return;
    g_flushing = 1;
    if (persist_write_snapshot() == 0) g_dirty = 0;
    g_flushing = 0;
}

void persist_mark_dirty(void) {
    if (!g_enabled || g_loading) return;
    g_dirty = 1;
    persist_flush();
}
