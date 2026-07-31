#include "shell.h"
#include "tty.h"
#include "../driver/keyboard.h"
#include "../driver/timer.h"
#include "../driver/usb_hid_kbd.h"
#include "fs/ramfs.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "process/task.h"
#include "lib/string.h"
#include "lib/kprintf.h"

#define CMD_MAX 76
#define MAX_ARGS 12

static char g_cmd[CMD_MAX + 1];
static int g_len;
static char g_cwd[RAMFS_MAX_PATH] = "/";
static char *g_argv[MAX_ARGS];
static int g_argc;

static void print_ok(const char *s) { tty_write_color(s, TTY_C_OK); }
static void print_err(const char *s) { tty_write_color(s, TTY_C_ERR); }
static void print_info(const char *s) { tty_write_color(s, TTY_C_INFO); }

static void tokenize(char *buf) {
    char *p = buf;
    g_argc = 0;
    while (*p == ' ') p++;
    while (*p && g_argc < MAX_ARGS) {
        g_argv[g_argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) {
            *p++ = '\0';
            while (*p == ' ') p++;
        }
    }
}

static int resolve_path(const char *in, char *out) {
    return ramfs_resolve(g_cwd, (in && *in) ? in : ".", out, RAMFS_MAX_PATH);
}

static const char *task_state_name(TaskState state) {
    switch (state) {
        case TASK_READY: return "READY";
        case TASK_RUNNING: return "RUN";
        case TASK_BLOCKED: return "SLEEP";
        case TASK_ZOMBIE: return "ZOMBIE";
        default: return "?";
    }
}

static void cmd_help(void) {
    print_info("ALOS clean kernel - commandes:");
    tty_write("help clear uname uptime mem ps usb pwd cd ls dir cat mkdir rm touch echo");
}

static void cmd_uname(void) {
    tty_write("ALOS clean-kernel i386 - build linux-like sans emulateurs");
}

static void cmd_uptime(void) {
    char line[64];
    uint32_t ms = timer_ms();
    ksprintf(line, "uptime: %u.%03u s", (unsigned)(ms / 1000u), (unsigned)(ms % 1000u));
    tty_write(line);
}

static void cmd_mem(void) {
    char line[96];
    ksprintf(line, "pmm: total=%u pages used=%u free=%u",
             (unsigned)pmm_total_pages(),
             (unsigned)pmm_used_pages(),
             (unsigned)pmm_free_pages());
    tty_write(line);
    ksprintf(line, "heap: used=%u KiB free=%u KiB largest=%u KiB blocks=%u",
             (unsigned)(heap_used() / 1024u),
             (unsigned)(heap_free() / 1024u),
             (unsigned)(heap_largest_free() / 1024u),
             (unsigned)heap_block_count());
    tty_write(line);
}

static void cmd_ps(void) {
    Task *tasks = task_table();
    char line[96];
    tty_write("PID  STATE   NAME");
    for (int i = 0; i < MAX_TASKS; ++i) {
        if (!tasks[i].name[0]) continue;
        ksprintf(line, "%u    %s   %s",
                 (unsigned)tasks[i].pid,
                 task_state_name(tasks[i].state),
                 tasks[i].name);
        tty_write(line);
    }
}

static void cmd_usb(void) {
    char line[96];
    usb_hid_kbd_status_string(line, sizeof(line));
    tty_write(line);
}

static void cmd_pwd(void) {
    tty_write(g_cwd);
}

static void cmd_cd(void) {
    char path[RAMFS_MAX_PATH];
    RamFSNode *n;
    if (g_argc < 2) {
        kstrncpy(g_cwd, "/", sizeof(g_cwd) - 1u);
        g_cwd[sizeof(g_cwd) - 1u] = '\0';
        return;
    }
    if (resolve_path(g_argv[1], path) != RAMFS_OK) {
        print_err("cd: chemin invalide");
        return;
    }
    n = ramfs_find(path);
    if (!ramfs_is_dir(n)) {
        print_err("cd: pas un dossier");
        return;
    }
    kstrncpy(g_cwd, path, sizeof(g_cwd) - 1u);
    g_cwd[sizeof(g_cwd) - 1u] = '\0';
}

static void cmd_ls(void) {
    char path[RAMFS_MAX_PATH];
    char out[1024];
    int rc;
    if (resolve_path((g_argc >= 2) ? g_argv[1] : ".", path) != RAMFS_OK) {
        print_err("ls: chemin invalide");
        return;
    }
    rc = ramfs_list_dir(path, out, sizeof(out));
    if (rc == RAMFS_ERR_NOTDIR) {
        RamFSNode *n = ramfs_find(path);
        if (n && !ramfs_is_dir(n)) {
            const char *name = path;
            for (const char *p = path; *p; ++p) {
                if (*p == '/' && p[1]) name = p + 1;
            }
            tty_write(name);
            return;
        }
        print_err("ls: pas un dossier");
        return;
    }
    if (rc < 0) {
        print_err("ls: introuvable");
        return;
    }
    tty_write(out[0] ? out : "(vide)");
}

static void cmd_cat(void) {
    char path[RAMFS_MAX_PATH];
    RamFSNode *n;
    char chunk[193];
    uint32_t off = 0;
    if (g_argc < 2) {
        print_err("cat: fichier manquant");
        return;
    }
    if (resolve_path(g_argv[1], path) != RAMFS_OK) {
        print_err("cat: chemin invalide");
        return;
    }
    n = ramfs_find(path);
    if (!n || ramfs_is_dir(n)) {
        print_err("cat: fichier introuvable");
        return;
    }
    while (off < n->size) {
        uint32_t ncopy = n->size - off;
        if (ncopy > sizeof(chunk) - 1u) ncopy = sizeof(chunk) - 1u;
        kmemcpy(chunk, ramfs_data(n) + off, ncopy);
        chunk[ncopy] = '\0';
        tty_write(chunk);
        off += ncopy;
    }
}

static void cmd_mkdir(void) {
    char path[RAMFS_MAX_PATH];
    if (g_argc < 2) {
        print_err("mkdir: nom manquant");
        return;
    }
    if (resolve_path(g_argv[1], path) != RAMFS_OK || ramfs_mkdir(path) != RAMFS_OK) {
        print_err("mkdir: echec");
        return;
    }
    print_ok("ok");
}

static void cmd_rm(void) {
    char path[RAMFS_MAX_PATH];
    if (g_argc < 2) {
        print_err("rm: cible manquante");
        return;
    }
    if (resolve_path(g_argv[1], path) != RAMFS_OK || ramfs_remove(path) != RAMFS_OK) {
        print_err("rm: echec");
        return;
    }
    print_ok("ok");
}

static void cmd_touch(void) {
    char path[RAMFS_MAX_PATH];
    if (g_argc < 2) {
        print_err("touch: nom manquant");
        return;
    }
    if (resolve_path(g_argv[1], path) != RAMFS_OK || !ramfs_create(path, "")) {
        print_err("touch: echec");
        return;
    }
    print_ok("ok");
}

static void cmd_echo(void) {
    char line[256];
    uint32_t pos = 0;
    for (int i = 1; i < g_argc; ++i) {
        const char *p = g_argv[i];
        if (i > 1 && pos + 1 < sizeof(line)) line[pos++] = ' ';
        while (*p && pos + 1 < sizeof(line)) line[pos++] = *p++;
    }
    line[pos] = '\0';
    tty_write(line);
}

static void run_command(void) {
    char local[CMD_MAX + 1];
    kstrncpy(local, g_cmd, sizeof(local) - 1u);
    local[sizeof(local) - 1u] = '\0';
    tokenize(local);
    if (!g_argc) return;

    if (kstrcmp(g_argv[0], "help") == 0) cmd_help();
    else if (kstrcmp(g_argv[0], "clear") == 0) tty_clear_output();
    else if (kstrcmp(g_argv[0], "uname") == 0) cmd_uname();
    else if (kstrcmp(g_argv[0], "uptime") == 0) cmd_uptime();
    else if (kstrcmp(g_argv[0], "mem") == 0) cmd_mem();
    else if (kstrcmp(g_argv[0], "ps") == 0) cmd_ps();
    else if (kstrcmp(g_argv[0], "usb") == 0) cmd_usb();
    else if (kstrcmp(g_argv[0], "pwd") == 0) cmd_pwd();
    else if (kstrcmp(g_argv[0], "cd") == 0) cmd_cd();
    else if (kstrcmp(g_argv[0], "ls") == 0 || kstrcmp(g_argv[0], "dir") == 0) cmd_ls();
    else if (kstrcmp(g_argv[0], "cat") == 0) cmd_cat();
    else if (kstrcmp(g_argv[0], "mkdir") == 0) cmd_mkdir();
    else if (kstrcmp(g_argv[0], "rm") == 0) cmd_rm();
    else if (kstrcmp(g_argv[0], "touch") == 0) cmd_touch();
    else if (kstrcmp(g_argv[0], "echo") == 0) cmd_echo();
    else print_err("commande inconnue; tape help");
}

void shell_run(void) {
    print_ok("ALOS clean kernel pret.");
    cmd_help();
    tty_clear_input();
    while (1) {
        keyboard_poll();
        usb_hid_kbd_poll();
        if (!keyboard_available()) {
            __asm__ volatile ("hlt");
            continue;
        }

        uint8_t key = keyboard_getkey();
        if (key == KEY_ENTER) {
            g_cmd[g_len] = '\0';
            tty_echo_cmd(g_cmd);
            run_command();
            g_len = 0;
            g_cmd[0] = '\0';
            tty_clear_input();
        } else if (key == KEY_BACKSPACE || key == KEY_DELETE) {
            if (g_len > 0) {
                g_len--;
                g_cmd[g_len] = '\0';
                tty_backspace(g_len);
            }
        } else if (key == KEY_ESCAPE) {
            g_len = 0;
            g_cmd[0] = '\0';
            tty_clear_input();
        } else if (key >= 32u && key < 127u && g_len < CMD_MAX) {
            g_cmd[g_len++] = (char)key;
            g_cmd[g_len] = '\0';
            tty_echo_char((char)key, g_len - 1);
        }
    }
}
