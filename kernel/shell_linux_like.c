#include "shell.h"
#include "tty.h"
#include "../driver/keyboard.h"
#include "../driver/timer.h"
#include "../driver/usb_hid_kbd.h"
#include "fs/ramfs.h"
#include "fs/diskfs.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "process/task.h"
#include "exec/asm_exec.h"
#include "exec/elf_loader.h"
#include "boot/bootinfo.h"
#include "net/net.h"
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

/* /mnt est le point de montage du systeme de fichiers persistant
 * (kernel/fs/diskfs.c) : tout chemin resolu sous /mnt est route vers
 * diskfs_* plutot que ramfs_* ci-dessous. */
static int is_mnt_path(const char *abs) {
    if (kstrcmp(abs, "/mnt") == 0) return 1;
    return kstrncmp(abs, "/mnt/", 5) == 0;
}
static const char *mnt_strip(const char *abs) {
    if (kstrcmp(abs, "/mnt") == 0) return "/";
    return abs + 4; /* saute "/mnt", garde le "/..." restant (ou "/") */
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
    tty_write("help clear uname uptime mem ps usb pwd cd ls dir cat mkdir rm touch echo asm");
    tty_write("elfrun <chemin>  elfmod <module>   (/mnt = systeme de fichiers persistant)");
    tty_write("elfrun3 <chemin>  elfmod3 <module>  (meme chose, mais en ring3/mode utilisateur)");
    tty_write("net info | net ping <ip>   (RTL8139 requis, voir README pour -device rtl8139)");
}

/* ── Mode 'asm' : REPL multi-lignes vers le micro-assembleur du noyau
 * (kernel/exec/asm_exec.c, deja compile mais jusque-la jamais expose).
 * Chaque ligne tapee est accumulee dans un buffer kmalloc ; une ligne
 * vide declenche l'assemblage + le lancement comme tache via
 * asmexec_run(). 'annuler' abandonne sans executer. ── */
#define ASM_BUF_CAP 2048
static int      g_asm_mode = 0;
static char    *g_asm_buf = 0;
static uint32_t g_asm_len = 0;
static int      g_asm_task_no = 0;

static void cmd_asm(void) {
    if (g_asm_buf) { print_err("asm: deja en cours de saisie"); return; }
    g_asm_buf = (char *)kmalloc(ASM_BUF_CAP);
    if (!g_asm_buf) { print_err("asm: memoire insuffisante"); return; }
    g_asm_buf[0] = '\0';
    g_asm_len = 0;
    g_asm_mode = 1;
    print_info("mode asm: une instruction i386 simplifiee par ligne.");
    print_info("ligne vide = assembler+lancer, 'annuler' = abandonner.");
}

static void asm_abort(const char *msg) {
    if (g_asm_buf) kfree(g_asm_buf);
    g_asm_buf = 0;
    g_asm_len = 0;
    g_asm_mode = 0;
    if (msg) print_info(msg);
}

static void asm_finish(void) {
    char errbuf[96];
    char name[16];
    int rc;

    g_asm_task_no++;
    ksprintf(name, "asm%d", g_asm_task_no);
    rc = asmexec_run(name, g_asm_buf, errbuf, sizeof(errbuf));
    if (rc == ASMEXEC_OK) {
        char l[96];
        ksprintf(l, "tache '%s' lancee (voir 'ps').", name);
        print_ok(l);
    } else {
        print_err(errbuf[0] ? errbuf : "asm: echec d'assemblage");
    }
    asm_abort(0);
}

static void asm_feed_line(const char *line) {
    uint32_t llen = kstrlen(line);
    if (g_asm_len + llen + 2 > ASM_BUF_CAP) {
        asm_abort("asm: programme trop long, saisie annulee");
        return;
    }
    kmemcpy(g_asm_buf + g_asm_len, line, llen);
    g_asm_len += llen;
    g_asm_buf[g_asm_len++] = '\n';
    g_asm_buf[g_asm_len] = '\0';
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
    if (g_argc < 2) {
        kstrncpy(g_cwd, "/", sizeof(g_cwd) - 1u);
        g_cwd[sizeof(g_cwd) - 1u] = '\0';
        return;
    }
    if (resolve_path(g_argv[1], path) != RAMFS_OK) {
        print_err("cd: chemin invalide");
        return;
    }
    if (is_mnt_path(path)) {
        if (!diskfs_available()) { print_err("cd: /mnt indisponible (pas de disque persistant)"); return; }
        if (!diskfs_is_dir(diskfs_find(mnt_strip(path)))) { print_err("cd: pas un dossier"); return; }
    } else if (!ramfs_is_dir(ramfs_find(path))) {
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
    if (is_mnt_path(path)) {
        if (!diskfs_available()) { print_err("ls: /mnt indisponible (pas de disque persistant)"); return; }
        rc = diskfs_list_dir(mnt_strip(path), out, sizeof(out));
        if (rc < 0) { print_err("ls: pas un dossier ou introuvable"); return; }
        tty_write(out[0] ? out : "(vide)");
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
    if (is_mnt_path(path)) {
        static uint8_t buf[DISKFS_MAX_FILE_SIZE];
        uint32_t total = 0;
        if (!diskfs_available()) { print_err("cat: /mnt indisponible (pas de disque persistant)"); return; }
        if (diskfs_read(mnt_strip(path), buf, sizeof(buf), &total) != DISKFS_OK) {
            print_err("cat: fichier introuvable");
            return;
        }
        while (off < total) {
            uint32_t ncopy = total - off;
            if (ncopy > sizeof(chunk) - 1u) ncopy = sizeof(chunk) - 1u;
            kmemcpy(chunk, buf + off, ncopy);
            chunk[ncopy] = '\0';
            tty_write(chunk);
            off += ncopy;
        }
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
    if (resolve_path(g_argv[1], path) != RAMFS_OK) {
        print_err("mkdir: echec");
        return;
    }
    if (is_mnt_path(path)) {
        if (!diskfs_available() || diskfs_mkdir(mnt_strip(path)) != DISKFS_OK) {
            print_err("mkdir: echec");
            return;
        }
        print_ok("ok");
        return;
    }
    if (ramfs_mkdir(path) != RAMFS_OK) {
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
    if (resolve_path(g_argv[1], path) != RAMFS_OK) {
        print_err("rm: echec");
        return;
    }
    if (is_mnt_path(path)) {
        if (!diskfs_available() || diskfs_remove(mnt_strip(path)) != DISKFS_OK) {
            print_err("rm: echec");
            return;
        }
        print_ok("ok");
        return;
    }
    if (ramfs_remove(path) != RAMFS_OK) {
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
    if (resolve_path(g_argv[1], path) != RAMFS_OK) {
        print_err("touch: echec");
        return;
    }
    if (is_mnt_path(path)) {
        if (!diskfs_available() || !diskfs_create_bytes(mnt_strip(path), "", 0)) {
            print_err("touch: echec");
            return;
        }
        print_ok("ok");
        return;
    }
    if (!ramfs_create(path, "")) {
        print_err("touch: echec");
        return;
    }
    print_ok("ok");
}

/* ── elfrun / elfmod : chargeur ELF32 (kernel/exec/elf_loader.c). elfrun
 * lit un fichier depuis ramfs ou /mnt (diskfs) ; elfmod charge directement
 * un module multiboot (meme mecanisme que l'installateur, sans limite de
 * taille liee aux fichiers ramfs/diskfs). ── */
static void cmd_elfrun(int ring3) {
    char path[RAMFS_MAX_PATH];
    static uint8_t buf[DISKFS_MAX_FILE_SIZE];
    uint32_t size = 0;
    char errbuf[96];
    char name[16];
    static int run_no = 0;
    int rc;
    const char *cmdname = ring3 ? "elfrun3" : "elfrun";

    if (g_argc < 2) { print_err("elfrun: chemin manquant"); return; }
    if (resolve_path(g_argv[1], path) != RAMFS_OK) { print_err("elfrun: chemin invalide"); return; }

    if (is_mnt_path(path)) {
        if (!diskfs_available() || diskfs_read(mnt_strip(path), buf, sizeof(buf), &size) != DISKFS_OK) {
            print_err("elfrun: fichier introuvable sur /mnt");
            return;
        }
    } else {
        RamFSNode *n = ramfs_find(path);
        if (!n || ramfs_is_dir(n)) { print_err("elfrun: fichier introuvable"); return; }
        size = n->size;
        if (size > sizeof(buf)) size = sizeof(buf);
        kmemcpy(buf, ramfs_data(n), size);
    }

    run_no++;
    ksprintf(name, "elf%d", run_no);
    rc = elf_loader_run(buf, size, name, ring3, errbuf, sizeof(errbuf));
    if (rc == ELF_OK) {
        char l[96];
        ksprintf(l, "tache '%s' lancee (%s, voir 'ps').", name, ring3 ? "ring3" : "ring0");
        print_ok(l);
    } else {
        print_err(errbuf[0] ? errbuf : cmdname);
    }
}

static void cmd_elfmod(int ring3) {
    const void *payload;
    uint32_t payload_size = 0;
    char errbuf[96];
    char name[16];
    static int run_no = 0;
    int rc;

    if (g_argc < 2) { print_err("elfmod: nom de module manquant"); return; }
    payload = bootinfo_find_module(g_argv[1], &payload_size);
    if (!payload || !payload_size) { print_err("elfmod: module multiboot introuvable"); return; }

    run_no++;
    ksprintf(name, "elfm%d", run_no);
    rc = elf_loader_run(payload, payload_size, name, ring3, errbuf, sizeof(errbuf));
    if (rc == ELF_OK) {
        char l[96];
        ksprintf(l, "tache '%s' lancee (%s, voir 'ps').", name, ring3 ? "ring3" : "ring0");
        print_ok(l);
    } else {
        print_err(errbuf[0] ? errbuf : "elfmod: echec de chargement");
    }
}

/* ── net : pile reseau minimale (kernel/net/net.c). "net info" affiche
 * l'etat de la carte, "net ping <ip>" resout l'adresse en ARP puis emet un
 * echo ICMP (bloquant, 2s de timeout). Voir net.h pour les limites
 * assumees (pas de DHCP/TCP/UDP, IP fixe, cache ARP a une seule entree). ── */
static int parse_ipv4(const char *s, uint8_t out[4]) {
    int part = 0, val = 0, digits = 0;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            digits++;
            if (val > 255 || digits > 3) return 0;
        } else if (*p == '.' || *p == '\0') {
            if (digits == 0 || part > 3) return 0;
            out[part++] = (uint8_t)val;
            val = 0; digits = 0;
            if (*p == '\0') break;
        } else {
            return 0;
        }
    }
    return part == 4;
}

static void cmd_net(void) {
    if (g_argc < 2) { print_err("net: sous-commande manquante (info|ping)"); return; }

    if (kstrcmp(g_argv[1], "info") == 0) {
        char l[96];
        if (!net_available()) { print_err("net: carte reseau absente (RTL8139 non detectee)"); return; }
        const uint8_t *mac = net_local_mac();
        const uint8_t *ip = net_local_ip();
        ksprintf(l, "mac=%x:%x:%x:%x:%x:%x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        tty_write(l);
        ksprintf(l, "ip=%d.%d.%d.%d (fixe, pas de DHCP)", ip[0], ip[1], ip[2], ip[3]);
        tty_write(l);
        return;
    }

    if (kstrcmp(g_argv[1], "ping") == 0) {
        uint8_t ip[4];
        int rtt;
        char l[96];
        if (g_argc < 3) { print_err("net ping: adresse IP manquante"); return; }
        if (!net_available()) { print_err("net: carte reseau absente (RTL8139 non detectee)"); return; }
        if (!parse_ipv4(g_argv[2], ip)) { print_err("net ping: adresse IPv4 invalide"); return; }
        rtt = net_ping(ip, 2000);
        if (rtt < 0) {
            print_err("net ping: timeout (pas de reponse ARP ou ICMP)");
        } else {
            ksprintf(l, "reponse de %s en %d ms", g_argv[2], rtt);
            print_ok(l);
        }
        return;
    }

    print_err("net: sous-commande inconnue (info|ping)");
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
    else if (kstrcmp(g_argv[0], "asm") == 0) cmd_asm();
    else if (kstrcmp(g_argv[0], "elfrun") == 0) cmd_elfrun(0);
    else if (kstrcmp(g_argv[0], "elfmod") == 0) cmd_elfmod(0);
    else if (kstrcmp(g_argv[0], "elfrun3") == 0) cmd_elfrun(1);
    else if (kstrcmp(g_argv[0], "elfmod3") == 0) cmd_elfmod(1);
    else if (kstrcmp(g_argv[0], "net") == 0) cmd_net();
    else print_err("commande inconnue; tape help");
}

void shell_run(void) {
    print_ok("ALOS clean kernel pret.");
    cmd_help();
    tty_clear_input();
    while (1) {
        keyboard_poll();
        usb_hid_kbd_poll();
        net_poll();
        if (!keyboard_available()) {
            __asm__ volatile ("hlt");
            continue;
        }

        uint8_t key = keyboard_getkey();
        if (key == KEY_ENTER) {
            g_cmd[g_len] = '\0';
            tty_echo_cmd(g_cmd);
            if (g_asm_mode) {
                if (g_len == 0) asm_finish();
                else if (kstrcmp(g_cmd, "annuler") == 0) asm_abort("asm: saisie annulee");
                else asm_feed_line(g_cmd);
            } else {
                run_command();
            }
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
