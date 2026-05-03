#ifndef KERNEL_FS_VFS_H
#define KERNEL_FS_VFS_H
#include <stdint.h>

#define VFS_MAX_FDS     16
#define VFS_MAX_PATH    64

typedef struct {
    int  (*read) (int fd, char *buf, uint32_t len);
    int  (*write)(int fd, const char *buf, uint32_t len);
    void (*close)(int fd);
} VFSOps;

typedef struct {
    int       valid;
    uint32_t  pos;
    void     *data;       /* pointeur vers le fichier (RamFS node) */
    VFSOps   *ops;
} FD;

void vfs_init(void);

/* fd 0=stdin, 1=stdout, 2=stderr déjà ouverts */
int  vfs_open (const char *path);
int  vfs_read (int fd, char *buf, uint32_t len);
int  vfs_write(int fd, const char *buf, uint32_t len);
void vfs_close(int fd);

#endif
