/* kernel/fs/diskfs.c - AlosFS : systeme de fichiers persistant avec
 * allocateur de blocs (v2). Voir diskfs.h pour le layout complet sur
 * disque. Binaire-compatible avec tools/alosfs_tool.py (utilitaire cote
 * hote, meme repo) : ne pas changer les offsets ci-dessous sans repercuter
 * le changement dans les deux. */
#include "diskfs.h"
#include "../lib/string.h"
#include "../../driver/ata.h"

#define DISKFS_PART_TYPE 0xA0  /* meme convention que kernel/install/installer.c */
#define DISKFS_SECTOR    512u

#define DISKFS_BLOCK_SIZE    DISKFS_SECTOR
#define DISKFS_BLOCK_PAYLOAD (DISKFS_BLOCK_SIZE - 4u)  /* 4 derniers octets = pointeur bloc suivant */
#define DISKFS_BLOCK_END     0xFFFFFFFFu

/* Plafond du nombre de blocs qu'un seul appel a diskfs_create_bytes() peut
 * allouer -- borne la pile utilisee pour la chaine temporaire pendant
 * l'ecriture, PAS une limite du format (voir diskfs.h). 256*508 ~= 127 Ko,
 * largement au-dessus de DISKFS_MAX_FILE_SIZE (le plafond cote appelants
 * shell/terminal). */
#define DISKFS_MAX_BLOCKS_PER_WRITE 256

static DiskFSNode nodes[DISKFS_MAX_NODES];
static int      g_region_start = -1; /* LBA absolu du debut de la zone AlosFS */
static int      g_available = 0;
static uint32_t g_bitmap_lba = 0;    /* LBA absolu du 1er secteur du bitmap */
static uint32_t g_data_lba = 0;      /* LBA absolu du bloc de donnees d'index 0 */
static uint32_t g_data_blocks = 0;   /* nombre total de blocs de donnees */

static int streq(const char *a, const char *b) { return kstrcmp(a, b) == 0; }
static uint32_t path_len(const char *p) { return kstrlen(p); }

static int path_has_prefix(const char *path, const char *prefix) {
    uint32_t lp = path_len(path), lx = path_len(prefix);
    if (lp < lx) return 0;
    return kstrncmp(path, prefix, lx) == 0;
}

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ── Persistance des noeuds : chaque noeud occupe son propre secteur,
 * immediatement apres le superbloc (secteur region_start+1+idx). ── */
static void node_to_sector(const DiskFSNode *n, uint8_t *sec) {
    kmemset(sec, 0, DISKFS_SECTOR);
    kstrncpy((char *)sec, n->name, DISKFS_MAX_PATH - 1);
    wr32(&sec[DISKFS_MAX_PATH], n->size);
    sec[DISKFS_MAX_PATH + 4] = n->used;
    sec[DISKFS_MAX_PATH + 5] = n->type;
    wr32(&sec[DISKFS_MAX_PATH + 8], n->first_block);
}

static void sector_to_node(DiskFSNode *n, const uint8_t *sec) {
    kmemset(n, 0, sizeof(*n));
    kmemcpy(n->name, sec, DISKFS_MAX_PATH - 1);
    n->name[DISKFS_MAX_PATH - 1] = '\0';
    n->size = rd32(&sec[DISKFS_MAX_PATH]);
    n->used = sec[DISKFS_MAX_PATH + 4];
    n->type = sec[DISKFS_MAX_PATH + 5];
    n->first_block = rd32(&sec[DISKFS_MAX_PATH + 8]);
}

static int save_node(int idx) {
    uint8_t sec[DISKFS_SECTOR];
    if (g_region_start < 0) return -1;
    node_to_sector(&nodes[idx], sec);
    return ata_write_sector((uint32_t)g_region_start + 1u + (uint32_t)idx, sec);
}

/* ── Bitmap de blocs libres : 1 bit/bloc, lu/ecrit secteur par secteur (pas
 * de cache RAM du bitmap complet -- reste correct meme sur un gros disque,
 * au prix d'un aller-retour disque par bit teste/modifie). ── */
static int bitmap_get(uint32_t block_idx) {
    uint32_t byte_off = block_idx / 8u;
    uint32_t sector_off = byte_off / DISKFS_SECTOR;
    uint32_t byte_in_sector = byte_off % DISKFS_SECTOR;
    uint8_t sec[DISKFS_SECTOR];
    if (ata_read_sector(g_bitmap_lba + sector_off, sec) != 0) return 1; /* erreur -> suppose occupe par securite */
    return (sec[byte_in_sector] >> (block_idx % 8u)) & 1u;
}

static int bitmap_set(uint32_t block_idx, int used) {
    uint32_t byte_off = block_idx / 8u;
    uint32_t sector_off = byte_off / DISKFS_SECTOR;
    uint32_t byte_in_sector = byte_off % DISKFS_SECTOR;
    uint8_t sec[DISKFS_SECTOR];
    if (ata_read_sector(g_bitmap_lba + sector_off, sec) != 0) return -1;
    if (used) sec[byte_in_sector] = (uint8_t)(sec[byte_in_sector] | (uint8_t)(1u << (block_idx % 8u)));
    else      sec[byte_in_sector] = (uint8_t)(sec[byte_in_sector] & (uint8_t)~(1u << (block_idx % 8u)));
    return ata_write_sector(g_bitmap_lba + sector_off, sec);
}

static int32_t alloc_block(void) {
    for (uint32_t i = 0; i < g_data_blocks; i++) {
        if (!bitmap_get(i)) {
            if (bitmap_set(i, 1) != 0) return -1;
            return (int32_t)i;
        }
    }
    return -1;
}

static void free_chain(uint32_t first_block) {
    uint32_t cur = first_block;
    while (cur != DISKFS_BLOCK_END) {
        uint8_t sec[DISKFS_SECTOR];
        uint32_t next = DISKFS_BLOCK_END;
        if (ata_read_sector(g_data_lba + cur, sec) == 0) {
            next = rd32(&sec[DISKFS_BLOCK_PAYLOAD]);
        }
        bitmap_set(cur, 0);
        cur = next;
    }
}

/* ── Recherche d'une partition de type 0xA0 dans le MBR du disque par
 * defaut. Retourne -1 si pas de table MBR valide (fallback disque brut,
 * gere par l'appelant). out_sectors recoit la taille de la partition (ou
 * du disque entier dans le cas du fallback, voir diskfs_init). ── */
static int find_persist_region(uint32_t *out_start_lba, uint32_t *out_sectors) {
    uint8_t mbr[DISKFS_SECTOR];
    if (ata_read_sector(0, mbr) != 0) return -1;
    if (!(mbr[510] == 0x55 && mbr[511] == 0xAA)) return -1;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = &mbr[446 + i * 16];
        uint8_t type = e[4];
        uint32_t start = rd32(&e[8]);
        uint32_t count = rd32(&e[12]);
        if (type == DISKFS_PART_TYPE && start != 0 && count != 0) {
            *out_start_lba = start;
            *out_sectors = count;
            return 0;
        }
    }
    return -1;
}

static int find_default_ata_index(void) {
    for (int i = 0; i < ATA_MAX_DEVICES; i++) {
        const AtaDeviceInfo *d = ata_get_device(i);
        if (d && d->present && !d->atapi) return i;
    }
    return -1;
}

void diskfs_init(void) {
    uint32_t region_start = 0, region_sectors = 0;
    uint8_t sec[DISKFS_SECTOR];

    g_available = 0;
    g_region_start = -1;
    kmemset(nodes, 0, sizeof(nodes));

    if (!ata_is_present()) return;

    if (find_persist_region(&region_start, &region_sectors) != 0) {
        int idx = find_default_ata_index();
        const AtaDeviceInfo *dev = (idx >= 0) ? ata_get_device(idx) : 0;
        if (!dev || !dev->sectors) return;
        region_start = 0; /* pas de table MBR : disque brut, tout depuis le debut */
        region_sectors = dev->sectors;
    }
    g_region_start = (int)region_start;

    if (ata_read_sector(region_start, sec) != 0) {
        g_region_start = -1;
        return;
    }

    if (sec[0] == 'A' && sec[1] == 'L' && sec[2] == 'F' && sec[3] == 'S') {
        /* Deja formate : relire le layout depuis le superbloc, puis la
         * table de noeuds. */
        g_bitmap_lba  = region_start + rd32(&sec[8]);
        g_data_lba    = region_start + rd32(&sec[12]);
        g_data_blocks = rd32(&sec[16]);
        for (int i = 0; i < DISKFS_MAX_NODES; i++) {
            uint8_t nb[DISKFS_SECTOR];
            if (ata_read_sector(region_start + 1u + (uint32_t)i, nb) != 0) continue;
            sector_to_node(&nodes[i], nb);
        }
    } else {
        /* Pas encore formate : calculer le layout, ecrire le superbloc, la
         * table de noeuds (TOUS les slots, pas seulement la racine -- sinon
         * un futur remontage relirait des metadonnees "used=1" fantomes
         * laissees par le contenu brut du disque) et un bitmap tout a zero
         * (tous les blocs libres). */
        uint32_t usable, bitmap_sectors, data_lba_rel, data_blocks;
        uint8_t zero[DISKFS_SECTOR];

        if (region_sectors < (2u + DISKFS_MAX_NODES)) { g_region_start = -1; return; } /* zone trop petite */

        usable = region_sectors - 1u - DISKFS_MAX_NODES;
        bitmap_sectors = (usable / 8u + (DISKFS_SECTOR - 1u)) / DISKFS_SECTOR;
        if (bitmap_sectors == 0) bitmap_sectors = 1;
        data_lba_rel = 1u + DISKFS_MAX_NODES + bitmap_sectors;
        if (data_lba_rel >= region_sectors) { g_region_start = -1; return; }
        data_blocks = region_sectors - data_lba_rel;

        kmemset(sec, 0, sizeof(sec));
        sec[0] = 'A'; sec[1] = 'L'; sec[2] = 'F'; sec[3] = 'S';
        sec[4] = 2; /* version */
        wr32(&sec[8],  1u + DISKFS_MAX_NODES); /* bitmap_lba relatif */
        wr32(&sec[12], data_lba_rel);          /* data_lba relatif */
        wr32(&sec[16], data_blocks);
        if (ata_write_sector(region_start, sec) != 0) { g_region_start = -1; return; }

        g_bitmap_lba  = region_start + 1u + DISKFS_MAX_NODES;
        g_data_lba    = region_start + data_lba_rel;
        g_data_blocks = data_blocks;

        kmemset(zero, 0, sizeof(zero));
        for (uint32_t i = 0; i < bitmap_sectors; i++) {
            if (ata_write_sector(g_bitmap_lba + i, zero) != 0) { g_region_start = -1; return; }
        }

        kmemset(nodes, 0, sizeof(nodes));
        nodes[0].used = 1;
        nodes[0].type = DISKFS_NODE_DIR;
        nodes[0].first_block = DISKFS_BLOCK_END;
        kstrncpy(nodes[0].name, "/", DISKFS_MAX_PATH - 1);
        for (int i = 1; i < DISKFS_MAX_NODES; i++) {
            nodes[i].first_block = DISKFS_BLOCK_END;
        }
        for (int i = 0; i < DISKFS_MAX_NODES; i++) {
            if (save_node(i) != 0) { g_region_start = -1; return; }
        }
    }

    g_available = 1;
}

int diskfs_available(void) { return g_available; }

int diskfs_resolve(const char *cwd, const char *path, char *out, uint32_t outsz) {
    char full[DISKFS_MAX_PATH];
    uint32_t fl = 0;

    if (!path || !*path || !out || outsz < 2) return DISKFS_ERR_INVAL;

    if (path[0] == '/') {
        while (*path && fl + 1 < sizeof(full)) full[fl++] = *path++;
    } else {
        const char *base = (cwd && *cwd) ? cwd : "/";
        if (base[0] != '/') return DISKFS_ERR_INVAL;
        while (*base && fl + 1 < sizeof(full)) full[fl++] = *base++;
        if (fl == 0) full[fl++] = '/';
        if (fl > 1 && full[fl - 1] != '/' && fl + 1 < sizeof(full)) full[fl++] = '/';
        while (*path && fl + 1 < sizeof(full)) full[fl++] = *path++;
    }
    full[fl] = '\0';

    out[0] = '/';
    out[1] = '\0';
    uint32_t ol = 1;

    uint32_t i = 0;
    while (full[i]) {
        while (full[i] == '/') i++;
        if (!full[i]) break;

        char tok[DISKFS_MAX_PATH];
        uint32_t tl = 0;
        while (full[i] && full[i] != '/') {
            if (tl + 1 < sizeof(tok)) tok[tl++] = full[i];
            i++;
        }
        tok[tl] = '\0';

        if (streq(tok, ".")) continue;
        if (streq(tok, "..")) {
            if (ol > 1) {
                while (ol > 1 && out[ol - 1] == '/') ol--;
                while (ol > 1 && out[ol - 1] != '/') ol--;
                if (ol > 1) ol--;
                out[ol] = '\0';
            }
            continue;
        }

        if (ol > 1) {
            if (ol + 1 >= outsz) return DISKFS_ERR_INVAL;
            out[ol++] = '/';
        }
        for (uint32_t k = 0; k < tl; k++) {
            if (ol + 1 >= outsz) return DISKFS_ERR_INVAL;
            out[ol++] = tok[k];
        }
        out[ol] = '\0';
    }

    if (ol == 0) {
        if (outsz < 2) return DISKFS_ERR_INVAL;
        out[0] = '/';
        out[1] = '\0';
    }

    return DISKFS_OK;
}

static int find_index_abs(const char *abs_path) {
    for (int i = 0; i < DISKFS_MAX_NODES; i++) {
        if (nodes[i].used && streq(nodes[i].name, abs_path)) return i;
    }
    return -1;
}

static int find_free_slot(void) {
    for (int i = 0; i < DISKFS_MAX_NODES; i++) {
        if (!nodes[i].used) return i;
    }
    return -1;
}

static int parent_of(const char *abs_path, char *out, uint32_t outsz) {
    uint32_t n = path_len(abs_path);
    if (!abs_path || !out || outsz < 2) return DISKFS_ERR_INVAL;
    if (n == 0 || abs_path[0] != '/') return DISKFS_ERR_INVAL;
    if (streq(abs_path, "/")) return DISKFS_ERR_INVAL;

    uint32_t i = n;
    while (i > 0 && abs_path[i - 1] != '/') i--;

    if (i <= 1) {
        out[0] = '/';
        out[1] = '\0';
        return DISKFS_OK;
    }

    if (i - 1 >= outsz) return DISKFS_ERR_INVAL;
    kmemcpy(out, abs_path, i - 1);
    out[i - 1] = '\0';
    return DISKFS_OK;
}

static int ensure_parent_dir_exists(const char *abs_path) {
    char parent[DISKFS_MAX_PATH];
    int pi;
    if (parent_of(abs_path, parent, sizeof(parent)) != DISKFS_OK) return DISKFS_ERR_INVAL;
    pi = find_index_abs(parent);
    if (pi < 0) return DISKFS_ERR_NOTFOUND;
    if (nodes[pi].type != DISKFS_NODE_DIR) return DISKFS_ERR_NOTDIR;
    return DISKFS_OK;
}

DiskFSNode *diskfs_find(const char *path) {
    char abs_path[DISKFS_MAX_PATH];
    if (!g_available) return 0;
    if (diskfs_resolve("/", path, abs_path, sizeof(abs_path)) != DISKFS_OK) return 0;
    int i = find_index_abs(abs_path);
    return (i >= 0) ? &nodes[i] : 0;
}

DiskFSNode *diskfs_create_bytes(const char *path, const void *content, uint32_t content_size) {
    char abs_path[DISKFS_MAX_PATH];
    int i;
    uint32_t nblocks;
    uint32_t chain[DISKFS_MAX_BLOCKS_PER_WRITE];

    if (!g_available) return 0;
    if (diskfs_resolve("/", path, abs_path, sizeof(abs_path)) != DISKFS_OK) return 0;
    if (streq(abs_path, "/")) return 0;

    nblocks = (content_size == 0) ? 0 : (content_size + DISKFS_BLOCK_PAYLOAD - 1u) / DISKFS_BLOCK_PAYLOAD;
    if (nblocks > DISKFS_MAX_BLOCKS_PER_WRITE) return 0;

    i = find_index_abs(abs_path);
    if (i >= 0) {
        if (nodes[i].type == DISKFS_NODE_DIR) return 0;
    } else {
        if (ensure_parent_dir_exists(abs_path) != DISKFS_OK) return 0;
        i = find_free_slot();
        if (i < 0) return 0;
        kmemset(&nodes[i], 0, sizeof(nodes[i]));
        nodes[i].used = 1;
        nodes[i].type = DISKFS_NODE_FILE;
        nodes[i].first_block = DISKFS_BLOCK_END;
        kstrncpy(nodes[i].name, abs_path, DISKFS_MAX_PATH - 1);
    }

    /* Liberer l'ancien contenu (le cas echeant) avant d'ecrire le nouveau. */
    if (nodes[i].first_block != DISKFS_BLOCK_END) {
        free_chain(nodes[i].first_block);
        nodes[i].first_block = DISKFS_BLOCK_END;
    }

    for (uint32_t b = 0; b < nblocks; b++) {
        int32_t blk = alloc_block();
        if (blk < 0) {
            for (uint32_t k = 0; k < b; k++) bitmap_set(chain[k], 0);
            return 0; /* plus assez d'espace libre */
        }
        chain[b] = (uint32_t)blk;
    }

    {
        const uint8_t *src = (const uint8_t *)content;
        uint32_t remaining = content_size;
        for (uint32_t b = 0; b < nblocks; b++) {
            uint8_t sec[DISKFS_BLOCK_SIZE];
            uint32_t chunk = remaining;
            if (chunk > DISKFS_BLOCK_PAYLOAD) chunk = DISKFS_BLOCK_PAYLOAD;
            kmemset(sec, 0, sizeof(sec));
            if (chunk && src) kmemcpy(sec, src, chunk);
            wr32(&sec[DISKFS_BLOCK_PAYLOAD], (b + 1u < nblocks) ? chain[b + 1u] : DISKFS_BLOCK_END);
            if (ata_write_sector(g_data_lba + chain[b], sec) != 0) {
                for (uint32_t k = 0; k < nblocks; k++) bitmap_set(chain[k], 0);
                return 0;
            }
            src += chunk;
            remaining -= chunk;
        }
    }

    nodes[i].first_block = (nblocks > 0) ? chain[0] : DISKFS_BLOCK_END;
    nodes[i].size = content_size;
    nodes[i].type = DISKFS_NODE_FILE;
    if (save_node(i) != 0) return 0;

    return &nodes[i];
}

int diskfs_read(const char *path, void *buf, uint32_t bufsize, uint32_t *out_size) {
    char abs_path[DISKFS_MAX_PATH];
    int i;
    uint32_t cur, copied, remaining;
    uint8_t *dst = (uint8_t *)buf;

    if (!g_available) return DISKFS_ERR_NODISK;
    if (diskfs_resolve("/", path, abs_path, sizeof(abs_path)) != DISKFS_OK) return DISKFS_ERR_INVAL;
    i = find_index_abs(abs_path);
    if (i < 0) return DISKFS_ERR_NOTFOUND;
    if (nodes[i].type != DISKFS_NODE_FILE) return DISKFS_ERR_NOTDIR;

    if (out_size) *out_size = nodes[i].size;

    cur = nodes[i].first_block;
    copied = 0;
    remaining = nodes[i].size;
    while (cur != DISKFS_BLOCK_END && remaining > 0) {
        uint8_t sec[DISKFS_BLOCK_SIZE];
        uint32_t chunk = remaining;
        if (chunk > DISKFS_BLOCK_PAYLOAD) chunk = DISKFS_BLOCK_PAYLOAD;
        if (ata_read_sector(g_data_lba + cur, sec) != 0) return DISKFS_ERR_IO;
        if (dst && copied < bufsize) {
            uint32_t to_copy = chunk;
            if (copied + to_copy > bufsize) to_copy = bufsize - copied;
            kmemcpy(dst + copied, sec, to_copy);
        }
        copied += chunk;
        remaining -= chunk;
        cur = rd32(&sec[DISKFS_BLOCK_PAYLOAD]);
    }
    return DISKFS_OK;
}

int diskfs_mkdir(const char *path) {
    char abs_path[DISKFS_MAX_PATH];
    int i, pr;

    if (!g_available) return DISKFS_ERR_NODISK;
    if (diskfs_resolve("/", path, abs_path, sizeof(abs_path)) != DISKFS_OK) return DISKFS_ERR_INVAL;
    if (streq(abs_path, "/")) return DISKFS_OK;

    i = find_index_abs(abs_path);
    if (i >= 0) return (nodes[i].type == DISKFS_NODE_DIR) ? DISKFS_OK : DISKFS_ERR_EXISTS;

    pr = ensure_parent_dir_exists(abs_path);
    if (pr != DISKFS_OK) return pr;

    i = find_free_slot();
    if (i < 0) return DISKFS_ERR_FULL;

    kmemset(&nodes[i], 0, sizeof(nodes[i]));
    nodes[i].used = 1;
    nodes[i].type = DISKFS_NODE_DIR;
    nodes[i].first_block = DISKFS_BLOCK_END;
    kstrncpy(nodes[i].name, abs_path, DISKFS_MAX_PATH - 1);
    if (save_node(i) != 0) return DISKFS_ERR_IO;
    return DISKFS_OK;
}

int diskfs_remove(const char *path) {
    char abs_path[DISKFS_MAX_PATH];
    int i;

    if (!g_available) return DISKFS_ERR_NODISK;
    if (diskfs_resolve("/", path, abs_path, sizeof(abs_path)) != DISKFS_OK) return DISKFS_ERR_INVAL;
    if (streq(abs_path, "/")) return DISKFS_ERR_INVAL;

    i = find_index_abs(abs_path);
    if (i < 0) return DISKFS_ERR_NOTFOUND;

    if (nodes[i].type == DISKFS_NODE_DIR) {
        char pref[DISKFS_MAX_PATH + 2];
        uint32_t lp = path_len(abs_path);
        if (lp + 2 >= sizeof(pref)) return DISKFS_ERR_INVAL;
        kmemcpy(pref, abs_path, lp);
        pref[lp] = '/';
        pref[lp + 1] = '\0';
        for (int j = 0; j < DISKFS_MAX_NODES; j++) {
            if (!nodes[j].used || j == i) continue;
            if (path_has_prefix(nodes[j].name, pref)) return DISKFS_ERR_DIR_NOTEMPTY;
        }
    } else if (nodes[i].first_block != DISKFS_BLOCK_END) {
        free_chain(nodes[i].first_block);
    }

    kmemset(&nodes[i], 0, sizeof(nodes[i]));
    nodes[i].first_block = DISKFS_BLOCK_END;
    if (save_node(i) != 0) return DISKFS_ERR_IO;
    return DISKFS_OK;
}

int diskfs_list_dir(const char *dir_path, char *buf, uint32_t bufsize) {
    char dir[DISKFS_MAX_PATH];
    uint32_t n = 0;
    uint32_t ldir;

    if (!buf || bufsize == 0) return DISKFS_ERR_INVAL;
    if (!g_available) { buf[0] = '\0'; return DISKFS_ERR_NODISK; }
    if (diskfs_resolve("/", dir_path ? dir_path : "/", dir, sizeof(dir)) != DISKFS_OK) return DISKFS_ERR_INVAL;

    {
        DiskFSNode *d = diskfs_find(dir);
        if (!d || d->type != DISKFS_NODE_DIR) return DISKFS_ERR_NOTDIR;
    }

    ldir = path_len(dir);

    for (int i = 0; i < DISKFS_MAX_NODES; i++) {
        if (!nodes[i].used) continue;
        if (streq(nodes[i].name, dir)) continue;

        const char *rest = 0;
        if (streq(dir, "/")) {
            if (nodes[i].name[0] != '/') continue;
            rest = nodes[i].name + 1;
        } else {
            char pref[DISKFS_MAX_PATH + 2];
            if (ldir + 2 >= sizeof(pref)) continue;
            kmemcpy(pref, dir, ldir);
            pref[ldir] = '/';
            pref[ldir + 1] = '\0';
            if (!path_has_prefix(nodes[i].name, pref)) continue;
            rest = nodes[i].name + ldir + 1;
        }
        if (!rest || !*rest) continue;

        int direct = 1;
        for (const char *p = rest; *p; p++) if (*p == '/') { direct = 0; break; }
        if (!direct) continue;

        uint32_t lr = path_len(rest);
        uint32_t need = lr + 1 + ((nodes[i].type == DISKFS_NODE_DIR) ? 1 : 0);
        if (n + need >= bufsize) break;

        kmemcpy(buf + n, rest, lr);
        n += lr;
        if (nodes[i].type == DISKFS_NODE_DIR) buf[n++] = '/';
        buf[n++] = '\n';
    }

    if (n >= bufsize) n = bufsize - 1;
    buf[n] = '\0';
    return (int)n;
}

int diskfs_node_capacity(void) { return DISKFS_MAX_NODES; }

const DiskFSNode *diskfs_node_at(int idx) {
    if (idx < 0 || idx >= DISKFS_MAX_NODES) return 0;
    return &nodes[idx];
}
