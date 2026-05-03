/* kernel/fs/vfs.c — Virtual File System (stdin/stdout/stderr + RamFS) */
#include "vfs.h"
#include "ramfs.h"
#include "../tty.h"
#include "../lib/string.h"
#include "../../driver/keyboard.h"

static FD fds[VFS_MAX_FDS];

/* ─── Opérations stdout/stderr ─────────────────────────────────────────── */
static int stdout_write(int fd, const char *buf, uint32_t len) {
    (void)fd;
    char tmp[512]; uint32_t n = len < 511 ? len : 511;
    kmemcpy(tmp, buf, n); tmp[n] = '\0';
    tty_write(tmp);
    return (int)n;
}

/* ─── Opérations RamFS ──────────────────────────────────────────────────── */
static int ramfs_read_fd(int fd, char *buf, uint32_t len) {
    RamFSNode *node = (RamFSNode*)fds[fd].data;
    if (!node || node->type == RAMFS_NODE_DIR) return -1;
    uint32_t rem = node->size - fds[fd].pos;
    if (len > rem) len = rem;
    kmemcpy(buf, ramfs_data(node) + fds[fd].pos, len);
    fds[fd].pos += len;
    return (int)len;
}
static int ramfs_write_fd(int fd, const char *buf, uint32_t len) { (void)fd;(void)buf;(void)len; return -1; }
static void ramfs_close_fd(int fd) { fds[fd].valid = 0; }

static VFSOps ramfs_ops = { ramfs_read_fd, ramfs_write_fd, ramfs_close_fd };
static VFSOps stdout_ops = { 0, stdout_write, 0 };

void vfs_init(void) {
    kmemset(fds, 0, sizeof(fds));
    /* fd 0 = stdin (clavier — read bloquant géré par keyboard_getkey) */
    fds[0].valid = 1;
    /* fd 1 = stdout */
    fds[1].valid = 1; fds[1].ops = &stdout_ops;
    /* fd 2 = stderr (même que stdout) */
    fds[2].valid = 1; fds[2].ops = &stdout_ops;
}

int vfs_open(const char *path) {
    RamFSNode *node = ramfs_find(path);
    if (!node || node->type == RAMFS_NODE_DIR) return -1;
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (!fds[i].valid) {
            fds[i].valid = 1;
            fds[i].pos   = 0;
            fds[i].data  = node;
            fds[i].ops   = &ramfs_ops;
            return i;
        }
    }
    return -1;
}

int vfs_read(int fd, char *buf, uint32_t len) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fds[fd].valid) return -1;
    if (fd == 0) {
        /* stdin : lecture clavier caractère par caractère */
        uint32_t i = 0;
        while (i < len - 1) {
            uint8_t k = keyboard_getkey();
            if (k == '\n' || k == '\r') { buf[i++] = '\n'; break; }
            buf[i++] = (char)k;
        }
        buf[i] = '\0';
        return (int)i;
    }
    if (!fds[fd].ops || !fds[fd].ops->read) return -1;
    return fds[fd].ops->read(fd, buf, len);
}

int vfs_write(int fd, const char *buf, uint32_t len) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fds[fd].valid) return -1;
    if (!fds[fd].ops || !fds[fd].ops->write) return -1;
    return fds[fd].ops->write(fd, buf, len);
}

void vfs_close(int fd) {
    if (fd < 3 || fd >= VFS_MAX_FDS || !fds[fd].valid) return;
    if (fds[fd].ops && fds[fd].ops->close) fds[fd].ops->close(fd);
    fds[fd].valid = 0;
}
