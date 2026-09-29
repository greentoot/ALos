#ifndef KERNEL_FS_DISKFS_H
#define KERNEL_FS_DISKFS_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AlosFS : systeme de fichiers persistant simple, stocke sur disque ATA
 * via driver/ata.c. Monte a /mnt (voir kernel/shell_linux_like.c) : tout
 * ce qui est ecrit sous /mnt survit a un redemarrage, contrairement au
 * reste de l'arbre qui reste en RAM via ramfs (kernel/fs/ramfs.c).
 *
 * Format "v2" (cette session) : allocateur de blocs libres + fichiers en
 * chaine de blocs, remplace le v1 (slot fixe de 4 Ko par fichier). Layout
 * du superbloc (secteur region_start) :
 *   [0..3]   magic "ALFS"
 *   [4]      version (2)
 *   [8..11]  bitmap_lba  (relatif a region_start)
 *   [12..15] data_lba    (relatif a region_start)
 *   [16..19] data_blocks (nombre total de blocs de donnees)
 * Suivi de DISKFS_MAX_NODES secteurs de metadonnees (un par noeud, tous
 * ecrits explicitement au format initial pour eviter de lire des
 * metadonnees "used=1" fantomes issues de la corbeille du disque), puis du
 * bitmap de blocs libres (1 bit/bloc), puis de la zone de donnees.
 *
 * Un bloc de donnees fait DISKFS_BLOCK_SIZE octets (= 1 secteur) : les 4
 * derniers octets contiennent le pointeur (index de bloc) vers le bloc
 * suivant du meme fichier (DISKFS_BLOCK_END = fin de chaine), les
 * DISKFS_BLOCK_PAYLOAD octets restants sont les donnees du fichier. Une
 * taille de fichier n'est donc plus limitee a un slot fixe : elle ne
 * depend que de l'espace libre sur le disque (dans les limites de
 * DISKFS_MAX_BLOCKS_PER_WRITE, une limite d'implementation de
 * diskfs_create_bytes() -- voir diskfs.c -- pas du format lui-meme).
 *
 * Montage (diskfs_init) : on cherche une partition MBR de type 0xA0
 * (meme convention que kernel/install/installer.c, "INSTALLER_PART_TYPE_
 * PERSIST") sur le premier disque ATA non-ATAPI detecte. Si aucune table
 * MBR valide n'est trouvee, on utilise le disque entier tel quel depuis
 * le LBA 0 -- pratique pour tester avec une image disque brute attachee
 * directement (-hda alos_persist.img), sans passer par l'installeur, ou
 * avec l'utilitaire cote hote tools/alosfs_tool.py qui parle le meme
 * format. Si le secteur de depart ne contient pas deja une signature
 * AlosFS, on le formate (calcule et ecrit le layout bitmap/data ci-dessus,
 * une racine "/"). Si aucun disque ATA n'est present du tout,
 * diskfs_available() renvoie 0 : /mnt reste simplement absent, le reste du
 * systeme continue de fonctionner sur ramfs comme avant. */

#define DISKFS_MAX_NODES     128
#define DISKFS_MAX_PATH       80
/* Taille de buffer utilisee par les appelants simples (shell/terminal) qui
 * chargent un fichier entier en RAM avant de le traiter (cat, elfrun...).
 * Ce n'est PAS une limite du format sur disque (voir plus haut) : juste un
 * plafond pratique pour dimensionner leurs buffers statiques. */
#define DISKFS_MAX_FILE_SIZE 16384

typedef enum {
    DISKFS_NODE_NONE = 0,
    DISKFS_NODE_FILE = 1,
    DISKFS_NODE_DIR  = 2,
} DiskFSNodeType;

/* Metadonnees en memoire (les donnees des fichiers restent sur disque,
 * lues/ecrites a la demande via diskfs_read/diskfs_create_bytes). */
typedef struct {
    char     name[DISKFS_MAX_PATH]; /* chemin absolu SOUS le point de montage, ex "/foo/bar.txt" */
    uint32_t size;
    uint8_t  used;
    uint8_t  type;
    uint32_t first_block; /* index (dans la zone de donnees) du 1er bloc de la chaine, ou "fin de chaine" si fichier vide/dossier */
} DiskFSNode;

#define DISKFS_OK                0
#define DISKFS_ERR_INVAL        -1
#define DISKFS_ERR_NOTFOUND     -2
#define DISKFS_ERR_EXISTS       -3
#define DISKFS_ERR_NOTDIR       -5
#define DISKFS_ERR_DIR_NOTEMPTY -6
#define DISKFS_ERR_FULL         -7
#define DISKFS_ERR_NODISK       -8
#define DISKFS_ERR_IO           -9

void        diskfs_init(void);
int         diskfs_available(void);

int         diskfs_resolve(const char *cwd, const char *path, char *out, uint32_t outsz);
DiskFSNode *diskfs_find(const char *path);
DiskFSNode *diskfs_create_bytes(const char *path, const void *content, uint32_t size);

/* Lit jusqu'a bufsize octets du fichier vers buf. *out_size recoit le
 * nombre d'octets reellement disponibles dans le fichier (peut depasser
 * bufsize si le buffer fourni est trop petit : dans ce cas seuls les
 * bufsize premiers octets sont copies). */
int         diskfs_read(const char *path, void *buf, uint32_t bufsize, uint32_t *out_size);

int         diskfs_mkdir(const char *path);
int         diskfs_remove(const char *path);
int         diskfs_list_dir(const char *dir_path, char *buf, uint32_t bufsize);

int               diskfs_node_capacity(void);
const DiskFSNode *diskfs_node_at(int idx);

static inline int diskfs_is_dir(const DiskFSNode *n) {
    return n && n->used && n->type == DISKFS_NODE_DIR;
}

#ifdef __cplusplus
}
#endif

#endif
