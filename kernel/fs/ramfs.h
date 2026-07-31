#ifndef KERNEL_FS_RAMFS_H
#define KERNEL_FS_RAMFS_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RAMFS_MAX_NODES  256
#define RAMFS_MAX_PATH    96
#define RAMFS_MAX_SIZE  4096

typedef enum {
    RAMFS_NODE_NONE = 0,
    RAMFS_NODE_FILE = 1,
    RAMFS_NODE_DIR  = 2,
    RAMFS_NODE_RO   = 3,
} RamFSNodeType;

typedef struct {
    char          name[RAMFS_MAX_PATH];
    char          data[RAMFS_MAX_SIZE];
    const char   *ro_data;
    uint32_t      size;
    uint8_t       used;
    uint8_t       type;
} RamFSNode;

/* Codes retour utilitaires */
#define RAMFS_OK               0
#define RAMFS_ERR_INVAL       -1
#define RAMFS_ERR_NOTFOUND    -2
#define RAMFS_ERR_EXISTS      -3
#define RAMFS_ERR_RO          -4
#define RAMFS_ERR_NOTDIR      -5
#define RAMFS_ERR_DIR_NOTEMPTY -6
#define RAMFS_ERR_FULL        -7

void        ramfs_init(void);

/* Resolution de chemin style Linux (., .., relatif/absolu) */
int         ramfs_resolve(const char *cwd, const char *path, char *out, uint32_t outsz);

RamFSNode  *ramfs_find(const char *path);
RamFSNode  *ramfs_create(const char *path, const char *content);
RamFSNode  *ramfs_create_bytes(const char *path, const void *content, uint32_t size);
RamFSNode  *ramfs_create_ro(const char *path, const char *data, uint32_t size);

int         ramfs_mkdir(const char *path);
int         ramfs_remove(const char *path);

/* Liste les enfants directs d'un dossier (une entree par ligne). */
int         ramfs_list_dir(const char *dir_path, char *buf, uint32_t bufsize);

/* Compat: liste de / */
int         ramfs_list(char *buf, uint32_t bufsize);

/* Iteration helpers */
int         ramfs_node_capacity(void);
const RamFSNode *ramfs_node_at(int idx);
void        ramfs_clear_mutable(void);

static inline const char *ramfs_data(RamFSNode *n) {
    return n->ro_data ? n->ro_data : n->data;
}

static inline int ramfs_is_dir(const RamFSNode *n) {
    return n && n->used && n->type == RAMFS_NODE_DIR;
}

#ifdef __cplusplus
}
#endif

#endif
