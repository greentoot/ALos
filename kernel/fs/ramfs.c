/* kernel/fs/ramfs.c - RamFS hierarchique style Linux */
#include "ramfs.h"
#include "../lib/string.h"
#include "persist.h"

static RamFSNode nodes[RAMFS_MAX_NODES];

static int streq(const char *a, const char *b) { return kstrcmp(a, b) == 0; }

static uint32_t path_len(const char *p) { return kstrlen(p); }

static int path_has_prefix(const char *path, const char *prefix) {
    uint32_t lp = path_len(path), lx = path_len(prefix);
    if (lp < lx) return 0;
    return kstrncmp(path, prefix, lx) == 0;
}

static int find_free_slot(void) {
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!nodes[i].used) return i;
    }
    return -1;
}

static int find_index_abs(const char *abs_path) {
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (nodes[i].used && streq(nodes[i].name, abs_path)) return i;
    }
    return -1;
}

/* out = parent directory path for abs_path; returns 0 on success. */
static int parent_of(const char *abs_path, char *out, uint32_t outsz) {
    uint32_t n = path_len(abs_path);
    if (!abs_path || !out || outsz < 2) return RAMFS_ERR_INVAL;
    if (n == 0 || abs_path[0] != '/') return RAMFS_ERR_INVAL;
    if (streq(abs_path, "/")) return RAMFS_ERR_INVAL;

    uint32_t i = n;
    while (i > 0 && abs_path[i - 1] != '/') i--;

    if (i <= 1) {
        out[0] = '/';
        out[1] = '\0';
        return RAMFS_OK;
    }

    if (i - 1 >= outsz) return RAMFS_ERR_INVAL;
    kmemcpy(out, abs_path, i - 1);
    out[i - 1] = '\0';
    return RAMFS_OK;
}

static int ensure_parent_dir_exists(const char *abs_path) {
    char parent[RAMFS_MAX_PATH];
    if (parent_of(abs_path, parent, sizeof(parent)) != RAMFS_OK) return RAMFS_ERR_INVAL;
    int pi = find_index_abs(parent);
    if (pi < 0) return RAMFS_ERR_NOTFOUND;
    if (nodes[pi].type != RAMFS_NODE_DIR) return RAMFS_ERR_NOTDIR;
    return RAMFS_OK;
}

int ramfs_resolve(const char *cwd, const char *path, char *out, uint32_t outsz) {
    char full[RAMFS_MAX_PATH];
    uint32_t fl = 0;

    if (!path || !*path || !out || outsz < 2) return RAMFS_ERR_INVAL;

    if (path[0] == '/') {
        while (*path && fl + 1 < sizeof(full)) full[fl++] = *path++;
    } else {
        const char *base = (cwd && *cwd) ? cwd : "/";
        if (base[0] != '/') return RAMFS_ERR_INVAL;
        while (*base && fl + 1 < sizeof(full)) full[fl++] = *base++;
        if (fl == 0) full[fl++] = '/';
        if (fl > 1 && full[fl - 1] != '/' && fl + 1 < sizeof(full)) full[fl++] = '/';
        while (*path && fl + 1 < sizeof(full)) full[fl++] = *path++;
    }
    full[fl] = '\0';

    /* Normalize . and .. */
    out[0] = '/';
    out[1] = '\0';
    uint32_t ol = 1;

    uint32_t i = 0;
    while (full[i]) {
        while (full[i] == '/') i++;
        if (!full[i]) break;

        char tok[RAMFS_MAX_PATH];
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
            if (ol + 1 >= outsz) return RAMFS_ERR_INVAL;
            out[ol++] = '/';
        }
        for (uint32_t k = 0; k < tl; k++) {
            if (ol + 1 >= outsz) return RAMFS_ERR_INVAL;
            out[ol++] = tok[k];
        }
        out[ol] = '\0';
    }

    if (ol == 0) {
        if (outsz < 2) return RAMFS_ERR_INVAL;
        out[0] = '/';
        out[1] = '\0';
    }

    return RAMFS_OK;
}

RamFSNode *ramfs_find(const char *path) {
    char abs_path[RAMFS_MAX_PATH];
    if (ramfs_resolve("/", path, abs_path, sizeof(abs_path)) != RAMFS_OK) return 0;
    int i = find_index_abs(abs_path);
    return (i >= 0) ? &nodes[i] : 0;
}

RamFSNode *ramfs_create(const char *path, const char *content) {
    uint32_t len = content ? kstrlen(content) : 0;
    return ramfs_create_bytes(path, content ? content : "", len);
}

RamFSNode *ramfs_create_bytes(const char *path, const void *content, uint32_t content_size) {
    char abs_path[RAMFS_MAX_PATH];
    if (ramfs_resolve("/", path, abs_path, sizeof(abs_path)) != RAMFS_OK) return 0;
    if (streq(abs_path, "/")) return 0;

    int i = find_index_abs(abs_path);
    if (i >= 0) {
        if (nodes[i].type == RAMFS_NODE_DIR || nodes[i].type == RAMFS_NODE_RO) return 0;
        uint32_t len = content_size;
        if (len >= RAMFS_MAX_SIZE) len = RAMFS_MAX_SIZE - 1;
        if (len && content) kmemcpy(nodes[i].data, content, len);
        nodes[i].data[len] = '\0';
        nodes[i].size = len;
        nodes[i].ro_data = 0;
        nodes[i].type = RAMFS_NODE_FILE;
        persist_mark_dirty();
        return &nodes[i];
    }

    if (ensure_parent_dir_exists(abs_path) != RAMFS_OK) return 0;
    i = find_free_slot();
    if (i < 0) return 0;

    kmemset(&nodes[i], 0, sizeof(nodes[i]));
    nodes[i].used = 1;
    nodes[i].type = RAMFS_NODE_FILE;
    kstrncpy(nodes[i].name, abs_path, RAMFS_MAX_PATH - 1);

    uint32_t len = content_size;
    if (len >= RAMFS_MAX_SIZE) len = RAMFS_MAX_SIZE - 1;
    if (len && content) kmemcpy(nodes[i].data, content, len);
    nodes[i].data[len] = '\0';
    nodes[i].size = len;
    nodes[i].ro_data = 0;
    persist_mark_dirty();

    return &nodes[i];
}

RamFSNode *ramfs_create_ro(const char *path, const char *data, uint32_t size) {
    char abs_path[RAMFS_MAX_PATH];
    if (!data) return 0;
    if (ramfs_resolve("/", path, abs_path, sizeof(abs_path)) != RAMFS_OK) return 0;
    if (streq(abs_path, "/")) return 0;

    int i = find_index_abs(abs_path);
    if (i >= 0) {
        if (nodes[i].type == RAMFS_NODE_DIR) return 0;
        nodes[i].used = 1;
        nodes[i].type = RAMFS_NODE_RO;
        nodes[i].ro_data = data;
        nodes[i].size = size;
        nodes[i].data[0] = '\0';
        return &nodes[i];
    }

    if (ensure_parent_dir_exists(abs_path) != RAMFS_OK) return 0;
    i = find_free_slot();
    if (i < 0) return 0;

    kmemset(&nodes[i], 0, sizeof(nodes[i]));
    nodes[i].used = 1;
    nodes[i].type = RAMFS_NODE_RO;
    kstrncpy(nodes[i].name, abs_path, RAMFS_MAX_PATH - 1);
    nodes[i].ro_data = data;
    nodes[i].size = size;

    return &nodes[i];
}

int ramfs_mkdir(const char *path) {
    char abs_path[RAMFS_MAX_PATH];
    if (ramfs_resolve("/", path, abs_path, sizeof(abs_path)) != RAMFS_OK) return RAMFS_ERR_INVAL;

    if (streq(abs_path, "/")) return RAMFS_OK;

    int i = find_index_abs(abs_path);
    if (i >= 0) {
        if (nodes[i].type == RAMFS_NODE_DIR) return RAMFS_OK;
        return RAMFS_ERR_EXISTS;
    }

    int pr = ensure_parent_dir_exists(abs_path);
    if (pr != RAMFS_OK) return pr;

    i = find_free_slot();
    if (i < 0) return RAMFS_ERR_FULL;

    kmemset(&nodes[i], 0, sizeof(nodes[i]));
    nodes[i].used = 1;
    nodes[i].type = RAMFS_NODE_DIR;
    kstrncpy(nodes[i].name, abs_path, RAMFS_MAX_PATH - 1);
    persist_mark_dirty();
    return RAMFS_OK;
}

int ramfs_remove(const char *path) {
    char abs_path[RAMFS_MAX_PATH];
    if (ramfs_resolve("/", path, abs_path, sizeof(abs_path)) != RAMFS_OK) return RAMFS_ERR_INVAL;
    if (streq(abs_path, "/")) return RAMFS_ERR_INVAL;

    int i = find_index_abs(abs_path);
    if (i < 0) return RAMFS_ERR_NOTFOUND;

    if (nodes[i].type == RAMFS_NODE_RO) return RAMFS_ERR_RO;

    if (nodes[i].type == RAMFS_NODE_DIR) {
        char pref[RAMFS_MAX_PATH + 2];
        uint32_t lp = path_len(abs_path);
        if (lp + 2 >= sizeof(pref)) return RAMFS_ERR_INVAL;
        kmemcpy(pref, abs_path, lp);
        pref[lp] = '/';
        pref[lp + 1] = '\0';

        for (int j = 0; j < RAMFS_MAX_NODES; j++) {
            if (!nodes[j].used || j == i) continue;
            if (path_has_prefix(nodes[j].name, pref)) return RAMFS_ERR_DIR_NOTEMPTY;
        }
    }

    kmemset(&nodes[i], 0, sizeof(nodes[i]));
    persist_mark_dirty();
    return RAMFS_OK;
}

int ramfs_list_dir(const char *dir_path, char *buf, uint32_t bufsize) {
    char dir[RAMFS_MAX_PATH];
    uint32_t n = 0;

    if (!buf || bufsize == 0) return RAMFS_ERR_INVAL;
    if (ramfs_resolve("/", dir_path ? dir_path : "/", dir, sizeof(dir)) != RAMFS_OK) return RAMFS_ERR_INVAL;

    RamFSNode *d = ramfs_find(dir);
    if (!d || d->type != RAMFS_NODE_DIR) return RAMFS_ERR_NOTDIR;

    uint32_t ldir = path_len(dir);

    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!nodes[i].used) continue;
        if (streq(nodes[i].name, dir)) continue;

        const char *rest = 0;
        if (streq(dir, "/")) {
            if (nodes[i].name[0] != '/') continue;
            rest = nodes[i].name + 1;
        } else {
            char pref[RAMFS_MAX_PATH + 2];
            if (ldir + 2 >= sizeof(pref)) continue;
            kmemcpy(pref, dir, ldir);
            pref[ldir] = '/';
            pref[ldir + 1] = '\0';
            if (!path_has_prefix(nodes[i].name, pref)) continue;
            rest = nodes[i].name + ldir + 1;
        }

        if (!rest || !*rest) continue;

        /* enfant direct seulement */
        int direct = 1;
        for (const char *p = rest; *p; p++) {
            if (*p == '/') { direct = 0; break; }
        }
        if (!direct) continue;

        uint32_t lr = path_len(rest);
        uint32_t need = lr + 1 + ((nodes[i].type == RAMFS_NODE_DIR) ? 1 : 0);
        if (n + need >= bufsize) break;

        kmemcpy(buf + n, rest, lr);
        n += lr;
        if (nodes[i].type == RAMFS_NODE_DIR) buf[n++] = '/';
        buf[n++] = '\n';
    }

    if (n >= bufsize) n = bufsize - 1;
    buf[n] = '\0';
    return (int)n;
}

int ramfs_list(char *buf, uint32_t bufsize) {
    return ramfs_list_dir("/", buf, bufsize);
}

void ramfs_init(void) {
    kmemset(nodes, 0, sizeof(nodes));

    /* Racine */
    nodes[0].used = 1;
    nodes[0].type = RAMFS_NODE_DIR;
    kstrncpy(nodes[0].name, "/", RAMFS_MAX_PATH - 1);

    /* Architecture Linux-like */
    ramfs_mkdir("/bin");
    ramfs_mkdir("/boot");
    ramfs_mkdir("/dev");
    ramfs_mkdir("/etc");
    ramfs_mkdir("/home");
    ramfs_mkdir("/home/root");
    ramfs_mkdir("/proc");
    ramfs_mkdir("/tmp");
    ramfs_mkdir("/usr");
    ramfs_mkdir("/usr/bin");
    ramfs_mkdir("/var");
    ramfs_mkdir("/var/log");
    ramfs_mkdir("/games");

    ramfs_create("/etc/os-release",
        "NAME=ALOS\n"
        "ID=alos\n"
        "VERSION=0.5\n"
        "PRETTY_NAME=ALOS 0.5\n");

    ramfs_create("/home/root/readme.txt",
        "ALOS v0.5\n"
        "  vmls             -- voir fichiers Jack (.vm)\n"
        "  jack <jeu>       -- lancer le jeu\n"
        "  cd /home/root    -- changer de dossier\n"
        "  help             -- toutes les commandes\n");

    ramfs_create("/home/root/loop.asm",
        "; Compte de 10 a 0 dans ecx\n"
        "mov ecx, 10\nboucle:\ndec ecx\ncmp ecx, 0\njg boucle\nret\n");

    ramfs_create("/home/root/fib.asm",
        "; fib(10) dans eax\n"
        "mov ecx,10\nmov eax,0\nmov ebx,1\n"
        "fib_loop:\ncmp ecx,0\nje fib_done\nmov edx,eax\nadd eax,ebx\nmov ebx,edx\ndec ecx\njmp fib_loop\n"
        "fib_done:\nret\n");

    ramfs_create("/home/root/count.asm",
        "; Somme 0..99 dans eax\n"
        "mov eax,0\nmov ecx,0\nloop_start:\nadd eax,ecx\ninc ecx\ncmp ecx,100\njl loop_start\nret\n");

    ramfs_create("/home/root/fact.asm",
        "; 7! = 5040 dans eax\n"
        "mov eax,1\nmov ecx,7\nfact_loop:\ncmp ecx,1\njle fact_done\nimul eax,ecx\ndec ecx\njmp fact_loop\nfact_done:\nret\n");
}

int ramfs_node_capacity(void) {
    return RAMFS_MAX_NODES;
}

const RamFSNode *ramfs_node_at(int idx) {
    if (idx < 0 || idx >= RAMFS_MAX_NODES) return 0;
    return &nodes[idx];
}

void ramfs_clear_mutable(void) {
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!nodes[i].used) continue;
        if (nodes[i].type == RAMFS_NODE_RO) continue;
        if (streq(nodes[i].name, "/")) continue;
        kmemset(&nodes[i], 0, sizeof(nodes[i]));
    }
}
