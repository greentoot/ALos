/* kernel/syscall/syscall.c */
#include "syscall.h"
#include "../fs/vfs.h"
#include "../process/task.h"
#include "../lib/string.h"

uint32_t syscall_handler(uint32_t num,
                         uint32_t arg1,
                         uint32_t arg2,
                         uint32_t arg3)
{
    switch (num) {

    /* write(fd, buf, len) — vérifie que buf != NULL */
    case SYS_WRITE: {
        if (arg2 == 0) return (uint32_t)-1;   /* NULL buf → erreur propre */
        return (uint32_t)vfs_write((int)arg1, (const char *)arg2, arg3);
    }

    /* exit(code) — marque zombie + boucle hlt (ne revient JAMAIS) */
    case SYS_EXIT: {
        task_kill(task_current()->pid);
        __asm__ volatile ("cli");
        while (1) __asm__ volatile ("hlt");
        return 0;   /* jamais atteint */
    }

    case SYS_GETPID:
        return task_current()->pid;

    case SYS_SLEEP:
        task_sleep(arg1);
        return 0;

    case SYS_OPEN:
        if (arg1 == 0) return (uint32_t)-1;
        return (uint32_t)vfs_open((const char *)arg1);

    case SYS_READ:
        if (arg2 == 0) return (uint32_t)-1;
        return (uint32_t)vfs_read((int)arg1, (char *)arg2, arg3);

    default:
        return (uint32_t)-1;
    }
}