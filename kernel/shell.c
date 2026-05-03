/* kernel/shell.c ??? kshell ALOS v0.5 */
#include "shell.h"
#include "tty.h"
#include "../driver/vga.h"
#include "../driver/gfx.h"
#include "../driver/keyboard.h"
#include "../driver/usb_xhci.h"
#include "../driver/usb_hid_kbd.h"
#include "../driver/audio.h"
#include "../driver/timer.h"

#include "memory/pmm.h"
#include "memory/heap.h"
#include "boot/bootinfo.h"
#include "process/task.h"
#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "fs/persist.h"
#include "install/installer.h"
#include "exec/asm_exec.h"
#include "exec/jackc.h"
#include "jack/jack_assets.h"
#include "jack/vm_interp.h"
#include "jack/vm_store.h"
#include "gba/gba_mgba.h"
#include "gba/gba_rominfo.h"
#include "nds/nds_backend.h"
#include "jack/font5x8.h"
#include "jack/cover_icon_pokemonrouge.h"
#include "jack/cover_icon_pokemonemeraude.h"
#include "lib/string.h"
#include "lib/kprintf.h"

#ifndef ALOS_HW_SAFE_GAMES
#define ALOS_HW_SAFE_GAMES 0
#endif

#define CMD_MAX 76
typedef struct HomeGame HomeGame;
static char cmd_buf[CMD_MAX + 1];
static int  cmd_len  = 0;
static int  cursor_x = 0;
static char g_cwd[RAMFS_MAX_PATH] = "/home/root";
static int  g_gba_compat_request = 0;
static void cmd_jack(void);
static void cmd_home(void);
static void cmd_disks(void);
static void cmd_install(void);
static void cmd_audio(void);
static void cmd_beep(void);
static int  launch_transparent_target(const char *target, int quiet_if_unknown);
static void home_draw_ui(const HomeGame *games, int ngames, int sel);
static const char *resolve_rom_blob_for_shell(const char *target,
                                              char *resolved_name,
                                              uint32_t resolved_name_sz,
                                              uint32_t *out_size,
                                              uint8_t *out_kind);

static int shell_has_mgba_bios(const char *rom_name, uint32_t *out_size) {
    return gba_mgba_has_bios_for_rom(rom_name, out_size);
}

/* ?????? Tokeniseur ????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????? */
#define MAX_ARGS 16
static char *argv[MAX_ARGS];
static int   argc;

static void tokenize(char *buf) {
    argc = 0;
    char *p = buf;
    while (*p == ' ') p++;
    while (*p && argc < MAX_ARGS) {
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) { *p++ = '\0'; while (*p == ' ') p++; }
    }
}

static void print_ok  (const char *s) { tty_write_color(s, TTY_C_OK);   }
static void print_err (const char *s) { tty_write_color(s, TTY_C_ERR);  }
static void print_info(const char *s) { tty_write_color(s, TTY_C_INFO); }
static void print_warn(const char *s) { tty_write_color(s, TTY_C_WARN); }

static const char *path_basename(const char *p) {
    const char *b = p;
    while (*p) { if (*p == '/') b = p + 1; p++; }
    return b;
}

static const char *path_ext(const char *p) {
    const char *e = 0;
    while (*p) { if (*p == '.') e = p; p++; }
    return e;
}

static int path_has_ext(const char *p, const char *ext) {
    const char *e = path_ext(p);
    return e && kstrcmp(e, ext) == 0;
}

static int resolve_user_path(const char *in, char *out) {
    if (!in || !*in) return ramfs_resolve(g_cwd, ".", out, RAMFS_MAX_PATH);
    return ramfs_resolve(g_cwd, in, out, RAMFS_MAX_PATH);
}

static int is_read_only_node(const RamFSNode *n) {
    return n && n->type == RAMFS_NODE_RO;
}

/* -----------------------------------------------------------------
 * Color home launcher (portable-console inspired UI)
 * ----------------------------------------------------------------- */
#define HOME_W            512
#define HOME_H            256
#define HOME_MAX_GAMES     64
#define HOME_COLS           4
#define HOME_ROWS           2
#define HOME_PAGE_SLOTS   (HOME_COLS * HOME_ROWS)
#define HOME_GRID_X        42
#define HOME_GRID_Y        46
#define HOME_TILE_W        98
#define HOME_TILE_H        58
#define HOME_TILE_STEP_X  104
#define HOME_TILE_STEP_Y   62
#define HOME_GRID_PANEL_X  30
#define HOME_GRID_PANEL_Y  38
#define HOME_GRID_PANEL_W 452
#define HOME_GRID_PANEL_H 132
#define HOME_BOTTOM_X      30
#define HOME_BOTTOM_Y     176
#define HOME_BOTTOM_W     452
#define HOME_BOTTOM_H      68
#define HOME_PREVIEW_W     86
#define HOME_PREVIEW_H     86
#define HOME_PREVIEW_XPAD   5
#define HOME_PREVIEW_YPAD   6
#define HOME_TILE_PREVIEW_W 42
#define HOME_TILE_PREVIEW_H 42
#define HC_COVER_GEN_BASE   32
#define HC_COVER_GEN_COLORS 16
#define HC_COVER_BASE_PR    64
#define HC_COVER_BASE_PE    (HC_COVER_BASE_PR + PR_COVER_COLORS)

static const uint8_t g_home_cover_gen_pal[HC_COVER_GEN_COLORS][3] = {
    { 0,  0,  0},
    {63, 63, 63},
    {56, 58, 59},
    {48, 49, 51},
    {36, 38, 40},
    {24, 26, 28},
    {51, 12, 13},
    {57, 28,  9},
    {59, 49, 11},
    {31, 43, 15},
    {11, 33, 18},
    {13, 40, 48},
    {14, 23, 47},
    {31, 23, 47},
    {41, 26, 15},
    {56, 40, 30},
};

enum {
    HC_BLACK = 0,
    HC_WHITE = 1,
    HC_BG_TOP = 2,
    HC_BG_BOTTOM = 3,
    HC_TOP_BAR = 4,
    HC_TOP_BAR_EDGE = 5,
    HC_PANEL = 6,
    HC_PANEL_EDGE = 7,
    HC_TILE = 8,
    HC_TILE_EDGE = 9,
    HC_TILE_SEL = 10,
    HC_TILE_SEL_EDGE = 11,
    HC_TEXT = 12,
    HC_TEXT_DIM = 13,
    HC_ICON_BLUE = 14,
    HC_ICON_GREEN = 15,
    HC_ICON_ORANGE = 16,
    HC_ICON_RED = 17,
    HC_ICON_CYAN = 18,
    HC_ICON_YELLOW = 19,
    HC_BUTTON_LEFT = 20,
    HC_BUTTON_RIGHT = 21,
    HC_BUTTON_EDGE = 22,
    HC_TILE_SHADOW = 23,
    HC_PAGE_DOT = 24,
    HC_PAGE_DOT_SEL = 25,
    HC_TEXT_ACCENT = 26,
};

struct HomeGame {
    char title[VM_NAME_MAX];
    char launch[VM_NAME_MAX];
    char subtitle[48];
    char detail[48];
    uint8_t kind; /* 0=VM, 1=GB, 2=GBC, 3=GBA, 4=NDS, 5=3DS */
    uint8_t cover_id; /* 0=none, 1=PokemonRouge style, 2=PokemonEmeraude style */
};

enum {
    HOME_KIND_VM  = 0,
    HOME_KIND_GB  = 1,
    HOME_KIND_GBC = 2,
    HOME_KIND_GBA = 3,
    HOME_KIND_NDS = 4,
    HOME_KIND_3DS = 5,
};

static uint8_t g_home_screen[HOME_W * HOME_H];
static char g_home_notice[80];

static void home_setup_palette(void) {
    gfx_set_palette(HC_BLACK, 0, 0, 0);
    gfx_set_palette(HC_WHITE, 63, 63, 63);
    gfx_set_palette(HC_BG_TOP, 57, 59, 61);
    gfx_set_palette(HC_BG_BOTTOM, 51, 53, 55);
    gfx_set_palette(HC_TOP_BAR, 55, 57, 59);
    gfx_set_palette(HC_TOP_BAR_EDGE, 30, 32, 34);
    gfx_set_palette(HC_PANEL, 62, 62, 63);
    gfx_set_palette(HC_PANEL_EDGE, 36, 38, 40);
    gfx_set_palette(HC_TILE, 60, 61, 62);
    gfx_set_palette(HC_TILE_EDGE, 40, 42, 44);
    gfx_set_palette(HC_TILE_SEL, 53, 62, 55);
    gfx_set_palette(HC_TILE_SEL_EDGE, 14, 42, 18);
    gfx_set_palette(HC_TEXT, 6, 7, 8);
    gfx_set_palette(HC_TEXT_DIM, 24, 26, 28);
    gfx_set_palette(HC_ICON_BLUE, 15, 30, 58);
    gfx_set_palette(HC_ICON_GREEN, 11, 52, 19);
    gfx_set_palette(HC_ICON_ORANGE, 63, 36, 9);
    gfx_set_palette(HC_ICON_RED, 57, 11, 11);
    gfx_set_palette(HC_ICON_CYAN, 10, 48, 57);
    gfx_set_palette(HC_ICON_YELLOW, 62, 55, 8);
    gfx_set_palette(HC_BUTTON_LEFT, 61, 61, 62);
    gfx_set_palette(HC_BUTTON_RIGHT, 35, 58, 63);
    gfx_set_palette(HC_BUTTON_EDGE, 30, 32, 34);
    gfx_set_palette(HC_TILE_SHADOW, 47, 50, 53);
    gfx_set_palette(HC_PAGE_DOT, 35, 38, 42);
    gfx_set_palette(HC_PAGE_DOT_SEL, 18, 45, 60);
    gfx_set_palette(HC_TEXT_ACCENT, 16, 52, 18);
    for (int i = 0; i < HC_COVER_GEN_COLORS; i++) {
        gfx_set_palette((uint8_t)(HC_COVER_GEN_BASE + i),
                        g_home_cover_gen_pal[i][0],
                        g_home_cover_gen_pal[i][1],
                        g_home_cover_gen_pal[i][2]);
    }
    for (int i = 0; i < PR_COVER_COLORS; i++) {
        gfx_set_palette((uint8_t)(HC_COVER_BASE_PR + i),
                        pr_cover_pal[i][0],
                        pr_cover_pal[i][1],
                        pr_cover_pal[i][2]);
    }
    for (int i = 0; i < PE_COVER_COLORS; i++) {
        gfx_set_palette((uint8_t)(HC_COVER_BASE_PE + i),
                        pe_cover_pal[i][0],
                        pe_cover_pal[i][1],
                        pe_cover_pal[i][2]);
    }
}

static int has_suffix(const char *s, const char *suffix) {
    uint32_t ls = kstrlen(s), lf = kstrlen(suffix);
    if (ls < lf) return 0;
    return kstrcmp(s + (ls - lf), suffix) == 0;
}

static int has_prefix(const char *s, const char *prefix) {
    uint32_t lp = kstrlen(prefix);
    if (kstrlen(s) < lp) return 0;
    return kstrncmp(s, prefix, lp) == 0;
}

static uint8_t home_kind_from_name(const char *name) {
    if (!name) return 0xFF;
    if (has_suffix(name, ".gb")) return HOME_KIND_GB;
    if (has_suffix(name, ".gbc")) return HOME_KIND_GBC;
    if (has_suffix(name, ".gba")) return HOME_KIND_GBA;
    if (has_suffix(name, ".nds")) return HOME_KIND_NDS;
    if (has_suffix(name, ".3ds") || has_suffix(name, ".cia")) return HOME_KIND_3DS;
    return 0xFF;
}

static int home_kind_is_rom(uint8_t kind) {
    return kind >= HOME_KIND_GB && kind <= HOME_KIND_3DS;
}

static int home_kind_supported(uint8_t kind) {
    return kind == HOME_KIND_VM || kind == HOME_KIND_GB || kind == HOME_KIND_GBC || kind == HOME_KIND_GBA;
}

static int home_kind_actionable_in_home(uint8_t kind) {
    if (home_kind_supported(kind)) return 1;
    if (kind == HOME_KIND_NDS) return nds_backend_can_launch();
    return 0;
}

static int home_kind_sort_weight(uint8_t kind) {
    switch (kind) {
        case HOME_KIND_VM: return 0;
        case HOME_KIND_GB: return 1;
        case HOME_KIND_GBC: return 2;
        case HOME_KIND_GBA: return 3;
        case HOME_KIND_NDS: return 4;
        case HOME_KIND_3DS: return 5;
        default: return 99;
    }
}

static const char *home_kind_badge(uint8_t kind) {
    switch (kind) {
        case HOME_KIND_VM:  return "VM";
        case HOME_KIND_GB:  return "GB";
        case HOME_KIND_GBC: return "GBC";
        case HOME_KIND_GBA: return "GBA";
        case HOME_KIND_NDS: return "NDS";
        case HOME_KIND_3DS: return "3DS";
        default: return "ROM";
    }
}

static const char *home_kind_name(uint8_t kind) {
    switch (kind) {
        case HOME_KIND_VM:  return "Jack VM";
        case HOME_KIND_GB:  return "Game Boy";
        case HOME_KIND_GBC: return "Game Boy Color";
        case HOME_KIND_GBA: return "Game Boy Advance";
        case HOME_KIND_NDS: return "Nintendo DS";
        case HOME_KIND_3DS: return "Nintendo 3DS";
        default: return "ROM";
    }
}

static const char *home_primary_action_label(uint8_t kind) {
    if (kind == HOME_KIND_NDS) return "LANCER DS";
    if (kind == HOME_KIND_3DS) return "BIENTOT";
    return "DEMARRER";
}

static const char *home_primary_action_for_game(const HomeGame *game) {
    if (!game) return "DEMARRER";
    if (game->kind == HOME_KIND_NDS) {
        if (nds_backend_internal_ready()) return "LANCER DS";
        if (nds_backend_bridge_ready()) return "DEV HOST";
        return "PREVIEW";
    }
    return home_primary_action_label(game->kind);
}

static void home_notice_clear(void) {
    g_home_notice[0] = '\0';
}

static void home_notice_set(const char *msg) {
    if (!msg) {
        home_notice_clear();
        return;
    }
    kstrncpy(g_home_notice, msg, sizeof(g_home_notice) - 1);
    g_home_notice[sizeof(g_home_notice) - 1] = '\0';
}

static void home_format_size(char *out, uint32_t outsz, uint32_t size) {
    uint32_t mib = 1024u * 1024u;
    uint32_t kib = 1024u;
    if (!out || outsz == 0) return;
    if (size >= mib) {
        ksprintf(out, "%u MiB", (unsigned)((size + mib - 1u) / mib));
    } else {
        ksprintf(out, "%u KiB", (unsigned)((size + kib - 1u) / kib));
    }
}

static uint8_t home_cover_id_from_title(const char *title) {
    if (!title) return 0;
    if (kstrcmp(title, "PokemonRouge") == 0) return 1;
    if (kstrcmp(title, "PokemonRougeFeu") == 0) return 1;
    if (kstrcmp(title, "PokemonVersionRougeFeu") == 0) return 1;
    if (has_prefix(title, "PokemonRouge")) return 1;
    if (kstrcmp(title, "PokemonEmeraude") == 0) return 2;
    if (has_prefix(title, "PokemonEmeraude")) return 2;
    if (kstrcmp(title, "PokemonVersionEmeraude") == 0) return 2;
    return 0;
}

static void home_fill_vm_meta(HomeGame *game) {
    if (!game) return;
    kstrncpy(game->subtitle, "Jeu Jack VM", sizeof(game->subtitle) - 1);
    game->subtitle[sizeof(game->subtitle) - 1] = '\0';
    kstrncpy(game->detail, "ENTER: lancer", sizeof(game->detail) - 1);
    game->detail[sizeof(game->detail) - 1] = '\0';
}

static void home_fill_gba_meta(HomeGame *game,
                               const char *rom_data,
                               uint32_t rom_size,
                               int has_cover,
                               int bios_ok) {
    GbaRomInfo info;
    uint32_t mib;

    if (!game) return;
    gba_rom_info_init(&info);
    mib = (rom_size + ((1024u * 1024u) - 1u)) / (1024u * 1024u);
    if (mib == 0) mib = 1;

    if (gba_rom_info_parse(rom_data, rom_size, &info)) {
        if (kstrcmp(info.hw_desc, "NONE") == 0) {
            ksprintf(game->subtitle, "%s | %s", info.game_code[0] ? info.game_code : "----", info.save_desc);
        } else {
            ksprintf(game->subtitle, "%s | %s | %s",
                     info.game_code[0] ? info.game_code : "----",
                     info.save_desc,
                     info.hw_desc);
        }
        ksprintf(game->detail, "%u MiB | BIOS:%s | Cover:%s",
                 (unsigned)mib,
                 bios_ok ? "on" : "off",
                 has_cover ? "on" : "off");
        game->cover_id = has_cover ? info.cover_id : 0;
    } else {
        ksprintf(game->subtitle, "ROM GBA");
        ksprintf(game->detail, "%u MiB | BIOS:%s | Cover:%s",
                 (unsigned)mib,
                 bios_ok ? "on" : "off",
                 has_cover ? "on" : "off");
        if (has_cover) {
            game->cover_id = home_cover_id_from_title(game->title);
        }
    }
}

static void home_fill_generic_rom_meta(HomeGame *game,
                                       uint8_t kind,
                                       uint32_t rom_size,
                                       int has_cover,
                                       int bios_ok) {
    char size_buf[16];
    if (!game) return;
    home_format_size(size_buf, sizeof(size_buf), rom_size);
    if (home_kind_supported(kind)) {
        ksprintf(game->subtitle, "%s | backend natif", home_kind_badge(kind));
        ksprintf(game->detail, "%s | BIOS:%s | Cover:%s",
                 size_buf, bios_ok ? "on" : "off", has_cover ? "on" : "off");
    } else {
        ksprintf(game->subtitle, "%s | backend a integrer", home_kind_badge(kind));
        ksprintf(game->detail, "%s | Cover:%s", size_buf, has_cover ? "on" : "off");
    }
    if (has_cover) {
        game->cover_id = home_cover_id_from_title(game->title);
    }
}

static void home_fill_nds_meta(HomeGame *game,
                               const char *rom_data,
                               uint32_t rom_size,
                               int has_cover) {
    NdsRomInfo info;
    uint32_t bios7 = 0, bios9 = 0, fw = 0, keycfg = 0;
    int assets_ok = nds_backend_probe_assets(&bios7, &bios9, &fw, &keycfg);
    int bridge_ok = nds_backend_bridge_ready();
    int internal_ok = nds_backend_internal_ready();
    char size_buf[16];

    if (!game) return;
    home_format_size(size_buf, sizeof(size_buf), rom_size);
    nds_rom_info_init(&info);

    if (nds_rom_info_parse(rom_data, rom_size, &info)) {
        ksprintf(game->subtitle, "%s | %s",
                 info.game_code[0] ? info.game_code : "----",
                 assets_ok ? (internal_ok ? "core interne" : (bridge_ok ? "bridge dev" : "preview")) : "bios/fmw manquants");
        ksprintf(game->detail, "%s | FW:%s | DS:%s",
                 size_buf,
                 fw ? "on" : "off",
                 internal_ok ? "interne" : (bridge_ok ? "dev-host" : "stub"));
    } else {
        ksprintf(game->subtitle, "NDS | header invalide");
        ksprintf(game->detail, "%s | FW:%s | DS:%s",
                 size_buf,
                 fw ? "on" : "off",
                 internal_ok ? "interne" : (bridge_ok ? "dev-host" : "stub"));
    }
    if (has_cover) {
        game->cover_id = home_cover_id_from_title(game->title);
    }
}

static int collect_home_games(HomeGame *out, int cap) {
    int n = 0;
    int cap_slots = (int)vm_store_capacity();
    for (int i = 0; i < cap_slots && n < cap; i++) {
        const char *name = 0;
        const char *data = 0;
        uint32_t sz = 0;
        uint32_t csz = 0;
        uint32_t bios_sz = 0;
        int bios_ok = 0;
        uint32_t ln;
        uint32_t bn;
        const char *base;
        char launch_buf[VM_NAME_MAX];
        char cover_name[VM_NAME_MAX];
        uint8_t kind;
        if (!(data = vm_store_get_data(i, &name, &sz)) || !name) continue;
        kind = home_kind_from_name(name);

        ln = kstrlen(name);
        if (has_suffix(name, ".vmdir")) {
            bn = (ln >= 6) ? (ln - 6) : 0;
            if (bn == 0 || bn >= VM_NAME_MAX) continue;
            kstrncpy(launch_buf, name, bn);
            launch_buf[bn] = '\0';
            base = path_basename(launch_buf);
            kstrncpy(out[n].title, base, VM_NAME_MAX - 1);
            out[n].title[VM_NAME_MAX - 1] = '\0';
            kstrncpy(out[n].launch, launch_buf, VM_NAME_MAX - 1);
            out[n].launch[VM_NAME_MAX - 1] = '\0';
            out[n].kind = HOME_KIND_VM;
            out[n].cover_id = 0;
            home_fill_vm_meta(&out[n]);
        } else if (home_kind_is_rom(kind)) {
            base = path_basename(name);
            ln = kstrlen(base);
            if (has_suffix(name, ".gb")) bn = (ln >= 3) ? (ln - 3) : 0;
            else if (has_suffix(name, ".gbc") || has_suffix(name, ".gba") ||
                     has_suffix(name, ".nds") || has_suffix(name, ".3ds") ||
                     has_suffix(name, ".cia")) bn = (ln >= 4) ? (ln - 4) : 0;
            else bn = 0;
            if (bn == 0 || bn >= VM_NAME_MAX) continue;
            kstrncpy(out[n].title, base, bn);
            out[n].title[bn] = '\0';
            kstrncpy(out[n].launch, name, VM_NAME_MAX - 1);
            out[n].launch[VM_NAME_MAX - 1] = '\0';
            out[n].kind = kind;
            out[n].cover_id = 0;
            ksprintf(cover_name, "%s/cover.png", out[n].title);
            if (kind == HOME_KIND_GB || kind == HOME_KIND_GBC || kind == HOME_KIND_GBA) {
                bios_ok = shell_has_mgba_bios(name, &bios_sz);
            }
            if (kind == HOME_KIND_GBA) {
                home_fill_gba_meta(&out[n], data, sz, vm_store_find(cover_name, &csz) != 0, bios_ok);
            } else if (kind == HOME_KIND_NDS) {
                home_fill_nds_meta(&out[n], data, sz, vm_store_find(cover_name, &csz) != 0);
            } else {
                home_fill_generic_rom_meta(&out[n], kind, sz, vm_store_find(cover_name, &csz) != 0, bios_ok);
            }
        } else {
            continue;
        }
        n++;
    }

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            int wi = home_kind_sort_weight(out[i].kind);
            int wj = home_kind_sort_weight(out[j].kind);
            if (wj < wi || (wj == wi && kstrcmp(out[j].title, out[i].title) < 0)) {
                HomeGame t = out[i];
                out[i] = out[j];
                out[j] = t;
            }
        }
    }
    return n;
}

static inline void home_set_px(int x, int y, uint8_t col) {
    if ((unsigned)x >= HOME_W || (unsigned)y >= HOME_H) return;
    g_home_screen[y * HOME_W + x] = col;
}

static void home_clear(uint8_t col) {
    for (int i = 0; i < HOME_W * HOME_H; i++) g_home_screen[i] = col;
}

static void home_fill_rect(int x, int y, int w, int h, uint8_t col) {
    for (int yy = y; yy < y + h; yy++) {
        if ((unsigned)yy >= HOME_H) continue;
        for (int xx = x; xx < x + w; xx++) {
            if ((unsigned)xx >= HOME_W) continue;
            g_home_screen[yy * HOME_W + xx] = col;
        }
    }
}

static void home_rect(int x, int y, int w, int h, uint8_t col) {
    for (int xx = x; xx < x + w; xx++) {
        home_set_px(xx, y, col);
        home_set_px(xx, y + h - 1, col);
    }
    for (int yy = y; yy < y + h; yy++) {
        home_set_px(x, yy, col);
        home_set_px(x + w - 1, yy, col);
    }
}

static void home_fill_round_rect(int x, int y, int w, int h, uint8_t col) {
    home_fill_rect(x + 2, y, w - 4, h, col);
    home_fill_rect(x, y + 2, w, h - 4, col);
}

static void home_round_rect(int x, int y, int w, int h, uint8_t col) {
    home_rect(x + 2, y, w - 4, h, col);
    home_rect(x, y + 2, w, h - 4, col);
    home_set_px(x + 1, y + 1, col);
    home_set_px(x + w - 2, y + 1, col);
    home_set_px(x + 1, y + h - 2, col);
    home_set_px(x + w - 2, y + h - 2, col);
}

static void home_draw_char(int x, int y, char c, uint8_t col, int scale) {
    const uint8_t *g = hack_font5x8[(uint8_t)c];
    for (int gx = 0; gx < 5; gx++) {
        uint8_t bits = g[gx];
        for (int gy = 0; gy < 8; gy++) {
            if (!((bits >> gy) & 1)) continue;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    home_set_px(x + gx * scale + sx, y + gy * scale + sy, col);
                }
            }
        }
    }
}

static int home_text_chars(const char *s, int max_chars) {
    int n = 0;
    while (s[n] && n < max_chars) n++;
    return n;
}

static void home_draw_text(int x, int y, const char *s, uint8_t col, int scale, int max_chars) {
    int i = 0;
    while (s[i] && i < max_chars) {
        char c = s[i];
        if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
        home_draw_char(x + i * (6 * scale), y, c, col, scale);
        i++;
    }
}

static void home_draw_text_center(int x, int y, int w, const char *s, uint8_t col, int scale, int max_chars) {
    int chars = home_text_chars(s, max_chars);
    int tw = chars * 6 * scale;
    int tx = x + (w - tw) / 2;
    if (tx < x) tx = x;
    home_draw_text(tx, y, s, col, scale, max_chars);
}

static uint8_t home_icon_color_for(int idx) {
    switch (idx % 6) {
        case 0: return HC_ICON_GREEN;
        case 1: return HC_ICON_BLUE;
        case 2: return HC_ICON_ORANGE;
        case 3: return HC_ICON_RED;
        case 4: return HC_ICON_CYAN;
        default: return HC_ICON_YELLOW;
    }
}

static void home_draw_icon_symbol(int x, int y, int idx) {
    switch (idx % 6) {
        case 0: /* joystick */
            home_fill_rect(x + 6, y + 10, 11, 3, HC_BLACK);
            home_fill_rect(x + 10, y + 7, 3, 9, HC_BLACK);
            home_fill_rect(x + 15, y + 7, 3, 3, HC_BLACK);
            break;
        case 1: /* sword-ish */
            home_fill_rect(x + 11, y + 6, 2, 9, HC_BLACK);
            home_fill_rect(x + 9, y + 13, 6, 2, HC_BLACK);
            home_fill_rect(x + 10, y + 15, 4, 2, HC_BLACK);
            break;
        case 2: /* globe */
            home_rect(x + 7, y + 7, 11, 11, HC_BLACK);
            home_fill_rect(x + 12, y + 7, 1, 11, HC_BLACK);
            home_fill_rect(x + 7, y + 12, 11, 1, HC_BLACK);
            break;
        case 3: /* chat bubble */
            home_fill_rect(x + 7, y + 8, 10, 7, HC_BLACK);
            home_fill_rect(x + 11, y + 15, 3, 3, HC_BLACK);
            break;
        case 4: /* tool */
            home_fill_rect(x + 8, y + 8, 2, 9, HC_BLACK);
            home_fill_rect(x + 10, y + 10, 7, 2, HC_BLACK);
            home_fill_rect(x + 14, y + 8, 4, 3, HC_BLACK);
            break;
        default: /* exclamation */
            home_fill_rect(x + 11, y + 7, 2, 8, HC_BLACK);
            home_fill_rect(x + 11, y + 16, 2, 2, HC_BLACK);
            break;
    }
}

static void home_draw_icon_preview(int x, int y, int idx) {
    uint8_t ic = home_icon_color_for(idx);
    home_fill_round_rect(x, y, HOME_PREVIEW_W, HOME_PREVIEW_H, HC_WHITE);
    home_round_rect(x, y, HOME_PREVIEW_W, HOME_PREVIEW_H, HC_TILE_EDGE);
    home_fill_round_rect(x + 8, y + 8, HOME_PREVIEW_W - 16, HOME_PREVIEW_H - 16, ic);
    home_round_rect(x + 8, y + 8, HOME_PREVIEW_W - 16, HOME_PREVIEW_H - 16, HC_TILE_EDGE);
    home_draw_icon_symbol(x + (HOME_PREVIEW_W - 24) / 2, y + (HOME_PREVIEW_H - 24) / 2, idx);
}

static void home_draw_cover_icon(int x, int y, uint8_t cover_id) {
    if (cover_id == 1) {
        int ox = x + (HOME_PREVIEW_W - PR_COVER_W) / 2;
        int oy = y + (HOME_PREVIEW_H - PR_COVER_H) / 2;
        home_fill_round_rect(x, y, HOME_PREVIEW_W, HOME_PREVIEW_H, HC_WHITE);
        for (int yy = 0; yy < PR_COVER_H; yy++) {
            for (int xx = 0; xx < PR_COVER_W; xx++) {
                uint8_t pi = pr_cover_pix[yy * PR_COVER_W + xx];
                home_set_px(ox + xx, oy + yy, (uint8_t)(HC_COVER_BASE_PR + (pi % PR_COVER_COLORS)));
            }
        }
        home_round_rect(x, y, HOME_PREVIEW_W, HOME_PREVIEW_H, HC_TILE_EDGE);
    } else if (cover_id == 2) {
        int ox = x + (HOME_PREVIEW_W - PE_COVER_W) / 2;
        int oy = y + (HOME_PREVIEW_H - PE_COVER_H) / 2;
        home_fill_round_rect(x, y, HOME_PREVIEW_W, HOME_PREVIEW_H, HC_WHITE);
        for (int yy = 0; yy < PE_COVER_H; yy++) {
            for (int xx = 0; xx < PE_COVER_W; xx++) {
                uint8_t pi = pe_cover_pix[yy * PE_COVER_W + xx];
                home_set_px(ox + xx, oy + yy, (uint8_t)(HC_COVER_BASE_PE + (pi % PE_COVER_COLORS)));
            }
        }
        home_round_rect(x, y, HOME_PREVIEW_W, HOME_PREVIEW_H, HC_TILE_EDGE);
    } else {
        home_draw_icon_preview(x, y, 0);
    }
}

static void home_draw_cover_preview(int x, int y, const uint8_t *pixels, int w, int h) {
    int ox;
    int oy;
    if (!pixels || w <= 0 || h <= 0) return;

    home_fill_round_rect(x, y, HOME_PREVIEW_W, HOME_PREVIEW_H, HC_WHITE);
    ox = x + (HOME_PREVIEW_W - w) / 2;
    oy = y + (HOME_PREVIEW_H - h) / 2;
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            uint8_t pi = (uint8_t)(pixels[yy * w + xx] % HC_COVER_GEN_COLORS);
            home_set_px(ox + xx, oy + yy, (uint8_t)(HC_COVER_GEN_BASE + pi));
        }
    }
    home_round_rect(x, y, HOME_PREVIEW_W, HOME_PREVIEW_H, HC_TILE_EDGE);
}

static void home_draw_background(void) {
    home_clear(HC_BG_TOP);
    home_fill_rect(0, 160, HOME_W, HOME_H - 160, HC_BG_BOTTOM);
    home_fill_rect(0, 148, HOME_W, 12, HC_TOP_BAR);
    for (int y = 0; y < 184; y += 4) {
        for (int x = (y & 2) ? 2 : 0; x < HOME_W; x += 8) {
            home_set_px(x, y, HC_BG_BOTTOM);
        }
    }
}

static void home_draw_top_bar(int page, int page_count) {
    char txt[32];
    home_fill_round_rect(26, 8, 460, 24, HC_TOP_BAR);
    home_round_rect(26, 8, 460, 24, HC_TOP_BAR_EDGE);
    home_fill_rect(30, 12, 452, 1, HC_WHITE);
    home_draw_text(40, 15, "ALOS HOME", HC_TEXT, 1, 16);
    home_draw_text(105, 15, "GAMES", HC_TEXT_DIM, 1, 8);

    for (int i = 0; i < 8; i++) {
        uint8_t ic = home_icon_color_for(i);
        int x = 276 + i * 24;
        home_fill_round_rect(x, 11, 18, 18, HC_WHITE);
        home_round_rect(x, 11, 18, 18, HC_TILE_EDGE);
        home_fill_round_rect(x + 3, 14, 12, 12, ic);
        home_round_rect(x + 3, 14, 12, 12, HC_TILE_EDGE);
    }

    ksprintf(txt, "%d/%d", page + 1, page_count ? page_count : 1);
    home_draw_text(456, 15, txt, HC_TEXT, 1, 8);
}

static void home_draw_cover_preview_fit(int x,
                                        int y,
                                        int box_w,
                                        int box_h,
                                        const uint8_t *pixels,
                                        int w,
                                        int h) {
    if (!pixels || w <= 0 || h <= 0) return;
    home_fill_round_rect(x, y, box_w, box_h, HC_WHITE);
    for (int yy = 0; yy < box_h; yy++) {
        int sy = (yy * h) / box_h;
        for (int xx = 0; xx < box_w; xx++) {
            int sx = (xx * w) / box_w;
            uint8_t pi = (uint8_t)(pixels[sy * w + sx] % HC_COVER_GEN_COLORS);
            home_set_px(x + xx, y + yy, (uint8_t)(HC_COVER_GEN_BASE + pi));
        }
    }
    home_round_rect(x, y, box_w, box_h, HC_TILE_EDGE);
}

static void home_draw_cover_icon_fit(int x, int y, int box_w, int box_h, uint8_t cover_id) {
    if (cover_id == 1) {
        home_fill_round_rect(x, y, box_w, box_h, HC_WHITE);
        for (int yy = 0; yy < box_h; yy++) {
            int sy = (yy * PR_COVER_H) / box_h;
            for (int xx = 0; xx < box_w; xx++) {
                int sx = (xx * PR_COVER_W) / box_w;
                uint8_t pi = pr_cover_pix[sy * PR_COVER_W + sx];
                home_set_px(x + xx, y + yy, (uint8_t)(HC_COVER_BASE_PR + (pi % PR_COVER_COLORS)));
            }
        }
        home_round_rect(x, y, box_w, box_h, HC_TILE_EDGE);
    } else if (cover_id == 2) {
        home_fill_round_rect(x, y, box_w, box_h, HC_WHITE);
        for (int yy = 0; yy < box_h; yy++) {
            int sy = (yy * PE_COVER_H) / box_h;
            for (int xx = 0; xx < box_w; xx++) {
                int sx = (xx * PE_COVER_W) / box_w;
                uint8_t pi = pe_cover_pix[sy * PE_COVER_W + sx];
                home_set_px(x + xx, y + yy, (uint8_t)(HC_COVER_BASE_PE + (pi % PE_COVER_COLORS)));
            }
        }
        home_round_rect(x, y, box_w, box_h, HC_TILE_EDGE);
    } else {
        home_fill_round_rect(x, y, box_w, box_h, HC_WHITE);
        home_round_rect(x, y, box_w, box_h, HC_TILE_EDGE);
        home_draw_icon_symbol(x + (box_w - 24) / 2, y + (box_h - 24) / 2, 0);
    }
}

static void home_draw_tile(int x, int y, const char *name, int selected, int icon_idx, uint8_t kind, uint8_t cover_id) {
    uint8_t fill = selected ? HC_TILE_SEL : HC_TILE;
    uint8_t edge = selected ? HC_TILE_SEL_EDGE : HC_TILE_EDGE;
    int preview_x = x + 6;
    int preview_y = y + 6;
    int title_x = x + 52;
    int title_y = y + 16;
    const uint8_t *cover_pixels = 0;
    int cover_w = 0;
    int cover_h = 0;

    if (!selected) {
        home_fill_round_rect(x + 2, y + 2, HOME_TILE_W, HOME_TILE_H, HC_TILE_SHADOW);
    }
    home_fill_round_rect(x, y, HOME_TILE_W, HOME_TILE_H, fill);
    home_round_rect(x, y, HOME_TILE_W, HOME_TILE_H, edge);
    if (selected) {
        home_round_rect(x + 1, y + 1, HOME_TILE_W - 2, HOME_TILE_H - 2, HC_WHITE);
        home_fill_rect(x + 4, y + 4, 4, 4, HC_TEXT_ACCENT);
    }

    if (jack_assets_get_cover_preview(name, &cover_pixels, &cover_w, &cover_h)) {
        home_draw_cover_preview_fit(preview_x, preview_y, HOME_TILE_PREVIEW_W, HOME_TILE_PREVIEW_H,
                                    cover_pixels, cover_w, cover_h);
    } else if (cover_id != 0) {
        home_draw_cover_icon_fit(preview_x, preview_y, HOME_TILE_PREVIEW_W, HOME_TILE_PREVIEW_H, cover_id);
    } else {
        home_fill_round_rect(preview_x, preview_y, HOME_TILE_PREVIEW_W, HOME_TILE_PREVIEW_H, HC_WHITE);
        home_round_rect(preview_x, preview_y, HOME_TILE_PREVIEW_W, HOME_TILE_PREVIEW_H, HC_TILE_EDGE);
        home_draw_icon_symbol(preview_x + (HOME_TILE_PREVIEW_W - 24) / 2,
                              preview_y + (HOME_TILE_PREVIEW_H - 24) / 2,
                              icon_idx + kind * 2);
    }

    home_draw_text(title_x, title_y, name, selected ? HC_TEXT : HC_TEXT_DIM, 1, 11);
    if (!selected) {
        home_fill_round_rect(x + 74, y + 46, 10, 4, HC_PAGE_DOT);
    }
    if (home_kind_is_rom(kind)) {
        const char *badge = home_kind_badge(kind);
        home_fill_round_rect(x + 72, y + 6, 20, 8, HC_ICON_ORANGE);
        home_round_rect(x + 72, y + 6, 20, 8, HC_TILE_EDGE);
        home_draw_text_center(x + 72, y + 7, 20, badge, HC_WHITE, 1, 3);
    }
}

static void home_draw_page_dots(int page, int page_count) {
    int dots = page_count;
    int start_x;
    int active = page;
    if (dots < 1) dots = 1;
    if (dots > 10) dots = 10;
    if (active < 0) active = 0;
    if (active >= dots) active = dots - 1;
    start_x = (HOME_W - (dots * 10 - 2)) / 2;
    for (int i = 0; i < dots; i++) {
        uint8_t c = (i == active) ? HC_PAGE_DOT_SEL : HC_PAGE_DOT;
        home_fill_round_rect(start_x + i * 10, 236, 8, 4, c);
        home_round_rect(start_x + i * 10, 236, 8, 4, HC_TILE_EDGE);
    }
}

static void home_draw_bottom_panel(const HomeGame *games, int sel, int page, int page_count) {
    char line[64];
    const char *action = home_primary_action_for_game(&games[sel]);
    int left_btn_x = HOME_BOTTOM_X + 28;
    int left_btn_y = HOME_BOTTOM_Y + 15;
    int right_btn_x = HOME_BOTTOM_X + HOME_BOTTOM_W - 152;
    int right_btn_y = HOME_BOTTOM_Y + 15;

    home_fill_round_rect(HOME_BOTTOM_X, HOME_BOTTOM_Y, HOME_BOTTOM_W, HOME_BOTTOM_H, HC_PANEL);
    home_round_rect(HOME_BOTTOM_X, HOME_BOTTOM_Y, HOME_BOTTOM_W, HOME_BOTTOM_H, HC_PANEL_EDGE);
    home_fill_rect(HOME_BOTTOM_X + 4, HOME_BOTTOM_Y + 4, HOME_BOTTOM_W - 8, 1, HC_WHITE);

    home_fill_round_rect(left_btn_x, left_btn_y, 124, 30, HC_BUTTON_LEFT);
    home_round_rect(left_btn_x, left_btn_y, 124, 30, HC_BUTTON_EDGE);
    home_draw_text_center(left_btn_x, left_btn_y + 9, 124, "AIDE (F1)", HC_TEXT, 1, 14);

    home_fill_round_rect(right_btn_x, right_btn_y, 124, 30, HC_BUTTON_RIGHT);
    home_round_rect(right_btn_x, right_btn_y, 124, 30, HC_BUTTON_EDGE);
    home_draw_text_center(right_btn_x, right_btn_y + 9, 124, action, HC_TEXT, 1, 14);

    ksprintf(line, games[sel].kind != HOME_KIND_VM ? "ROM: %s" : "Jeu: %s", games[sel].title);
    home_draw_text_center(HOME_BOTTOM_X + 160, HOME_BOTTOM_Y + 16, 156, line, HC_TEXT, 1, 24);
    home_draw_text_center(HOME_BOTTOM_X + 160, HOME_BOTTOM_Y + 28, 156, games[sel].subtitle, HC_TEXT, 1, 26);
    home_draw_text_center(HOME_BOTTOM_X + 160, HOME_BOTTOM_Y + 40, 156,
                          g_home_notice[0] ? g_home_notice : games[sel].detail,
                          g_home_notice[0] ? HC_TEXT_ACCENT : HC_TEXT_DIM,
                          1, 26);
    home_draw_page_dots(page, page_count);
}

static void home_draw_modal_shell(const char *title) {
    home_fill_round_rect(58, 34, 396, 164, HC_PANEL);
    home_fill_round_rect(61, 37, 396, 164, HC_TILE_SHADOW);
    home_fill_round_rect(58, 34, 396, 164, HC_PANEL);
    home_round_rect(58, 34, 396, 164, HC_PANEL_EDGE);
    home_fill_rect(62, 38, 388, 1, HC_WHITE);
    home_fill_round_rect(70, 46, 372, 18, HC_TOP_BAR);
    home_round_rect(70, 46, 372, 18, HC_TOP_BAR_EDGE);
    home_draw_text_center(70, 51, 372, title, HC_TEXT, 1, 28);
}

static void home_draw_modal_preview(int x, int y, const HomeGame *game) {
    const uint8_t *cover_pixels = 0;
    int cover_w = 0;
    int cover_h = 0;
    if (!game) return;
    if (jack_assets_get_cover_preview(game->title, &cover_pixels, &cover_w, &cover_h)) {
        home_draw_cover_preview(x, y, cover_pixels, cover_w, cover_h);
    } else if (game->cover_id != 0) {
        home_draw_cover_icon(x, y, game->cover_id);
    } else {
        home_draw_icon_preview(x, y, game->kind * 2);
    }
}

static void home_draw_nds_modal(const HomeGame *game, const NdsBackendStatus *status, const char *msg) {
    char line[80];
    char size_buf[16];

    home_draw_modal_shell("NDS PREFLIGHT");
    home_draw_modal_preview(78, 78, game);
    home_draw_text(176, 76, game->title, HC_TEXT, 1, 24);

    if (status && status->rom_valid) {
        home_format_size(size_buf, sizeof(size_buf), status->rom_size);
        ksprintf(line, "Code:%s  Maker:%s  Taille:%s",
                 status->rom.game_code[0] ? status->rom.game_code : "----",
                 status->rom.maker_code[0] ? status->rom.maker_code : "--",
                 size_buf);
        home_draw_text(176, 92, line, HC_TEXT_DIM, 1, 34);

        ksprintf(line, "ARM9 off:%u  size:%u",
                 (unsigned)status->rom.arm9_rom_offset,
                 (unsigned)status->rom.arm9_size);
        home_draw_text(176, 106, line, HC_TEXT_DIM, 1, 34);

        ksprintf(line, "ARM7 off:%u  size:%u",
                 (unsigned)status->rom.arm7_rom_offset,
                 (unsigned)status->rom.arm7_size);
        home_draw_text(176, 118, line, HC_TEXT_DIM, 1, 34);

        ksprintf(line, "BIOS7:%s  BIOS9:%s",
                 status->bios7_size ? "OK" : "KO",
                 status->bios9_size ? "OK" : "KO");
        home_draw_text(176, 132, line, status->assets_ready ? HC_TEXT_ACCENT : HC_TEXT, 1, 34);

        ksprintf(line, "FW:%s  key.cfg:%s",
                 status->firmware_size ? "OK" : "KO",
                 status->keycfg_size ? "OK" : "optionnel");
        home_draw_text(176, 144, line, status->assets_ready ? HC_TEXT_ACCENT : HC_TEXT, 1, 34);
    } else {
        home_draw_text(176, 96, "Header NDS invalide.", HC_TEXT, 1, 28);
    }

    home_draw_text(78, 172, msg ? msg : "Core DS non integre pour le moment.", HC_TEXT, 1, 58);
    home_draw_text(78, 184, "ESC/ENTREE: retour  |  F1: terminal", HC_TEXT_DIM, 1, 42);
}

static void home_draw_nds_session_modal(const HomeGame *game, const NdsBackendStatus *status, const char *msg) {
    char line[80];
    char size_buf[16];

    home_draw_modal_shell("NDS SESSION");
    home_draw_modal_preview(78, 78, game);
    home_draw_text(176, 76, game->title, HC_TEXT, 1, 24);

    if (status && status->rom_valid) {
        home_format_size(size_buf, sizeof(size_buf), status->rom_size);
        ksprintf(line, "Code:%s  Maker:%s  Taille:%s",
                 status->rom.game_code[0] ? status->rom.game_code : "----",
                 status->rom.maker_code[0] ? status->rom.maker_code : "--",
                 size_buf);
        home_draw_text(176, 92, line, HC_TEXT_DIM, 1, 34);
        ksprintf(line, "Bridge:%s  BIOS7:%s  BIOS9:%s",
                 nds_backend_bridge_ready() ? "OK" : "KO",
                 status->bios7_size ? "OK" : "KO",
                 status->bios9_size ? "OK" : "KO");
        home_draw_text(176, 106, line, HC_TEXT_DIM, 1, 34);
        ksprintf(line, "Firmware:%s  key.cfg:%s",
                 status->firmware_size ? "OK" : "KO",
                 status->keycfg_size ? "OK" : "optionnel");
        home_draw_text(176, 118, line, status->assets_ready ? HC_TEXT_ACCENT : HC_TEXT, 1, 34);
    } else {
        home_draw_text(176, 96, "Header NDS invalide.", HC_TEXT, 1, 28);
    }

    home_draw_text(78, 172, msg ? msg : "Session DS en preparation.", HC_TEXT, 1, 58);
    home_draw_text(78, 184, "Ferme la fenetre DS externe pour revenir a ALOS.", HC_TEXT_DIM, 1, 58);
}

static int home_show_nds_preflight(const HomeGame *games, int ngames, int sel) {
    char resolved[VM_NAME_MAX];
    char msg[176];
    uint8_t kind = 0xFF;
    uint32_t size = 0;
    const char *rom;
    NdsBackendStatus status;

    rom = resolve_rom_blob_for_shell(games[sel].launch, resolved, sizeof(resolved), &size, &kind);
    nds_backend_status_init(&status);
    if (!rom || kind != HOME_KIND_NDS) {
        kstrncpy(msg, "ROM NDS introuvable dans le store.", sizeof(msg) - 1);
        msg[sizeof(msg) - 1] = '\0';
    } else if (!nds_backend_preflight(resolved, rom, size, &status, msg, sizeof(msg))) {
        /* msg deja rempli */
    } else {
        if (nds_backend_internal_ready()) {
            kstrncpy(msg, "Assets DS prets. Le prochain pas est le branchement interne du core.", sizeof(msg) - 1);
        } else if (nds_backend_bridge_ready()) {
            kstrncpy(msg, "Assets DS prets. Bridge host disponible en mode dev uniquement.", sizeof(msg) - 1);
        } else {
            kstrncpy(msg, "Assets DS prets. Le core interne DS manque encore pour l'ISO autonome.", sizeof(msg) - 1);
        }
        msg[sizeof(msg) - 1] = '\0';
    }

    home_draw_ui(games, ngames, sel);
    home_draw_nds_modal(&games[sel], &status, msg);
    gfx_blit_indexed_512x256(g_home_screen);

    for (;;) {
        uint8_t key = keyboard_getkey();
        if (key == KEY_F1) {
            gfx_restore_text_mode();
            vga_init();
            tty_init();
            tty_write("Mode terminal ALOS (F1 pour revenir au menu).");
            return 1;
        }
        if (key == KEY_ESCAPE || key == KEY_ENTER) {
            home_notice_set(msg);
            home_draw_ui(games, ngames, sel);
            gfx_blit_indexed_512x256(g_home_screen);
            return 0;
        }
    }
}

static int home_run_nds_session(const HomeGame *games, int ngames, int sel) {
    char resolved[VM_NAME_MAX];
    char msg[176];
    uint8_t kind = 0xFF;
    uint32_t size = 0;
    const char *rom;
    NdsBackendStatus status;
    uint32_t request_id = 0;
    uint32_t launch_ms;
    int started = 0;

    rom = resolve_rom_blob_for_shell(games[sel].launch, resolved, sizeof(resolved), &size, &kind);
    nds_backend_status_init(&status);
    if (!rom || kind != HOME_KIND_NDS) {
        home_notice_set("ROM NDS introuvable dans le store.");
        home_draw_ui(games, ngames, sel);
        gfx_blit_indexed_512x256(g_home_screen);
        return 0;
    }
    if (!nds_backend_preflight(resolved, rom, size, &status, msg, sizeof(msg))) {
        home_notice_set(msg);
        home_draw_ui(games, ngames, sel);
        gfx_blit_indexed_512x256(g_home_screen);
        return 0;
    }
    if (nds_backend_begin_session(resolved, rom, size, &request_id, msg, sizeof(msg)) != 0) {
        home_notice_set(msg);
        home_draw_ui(games, ngames, sel);
        gfx_blit_indexed_512x256(g_home_screen);
        return 0;
    }

    kstrncpy(msg, "Ouverture de la fenetre Nintendo DS...", sizeof(msg) - 1);
    msg[sizeof(msg) - 1] = '\0';
    home_draw_ui(games, ngames, sel);
    home_draw_nds_session_modal(&games[sel], &status, msg);
    gfx_blit_indexed_512x256(g_home_screen);

    launch_ms = timer_ms();
    for (;;) {
        NdsBridgeEvent event;
        while (nds_backend_poll_bridge_event(&event)) {
            if (event.request_id != request_id) continue;
            if (event.kind == NDS_BRIDGE_EVENT_STARTED) {
                started = 1;
                kstrncpy(msg,
                         "Session DS active. Ferme la fenetre pour revenir a ALOS.",
                         sizeof(msg) - 1);
                msg[sizeof(msg) - 1] = '\0';
                home_draw_ui(games, ngames, sel);
                home_draw_nds_session_modal(&games[sel], &status, msg);
                gfx_blit_indexed_512x256(g_home_screen);
            } else if (event.kind == NDS_BRIDGE_EVENT_CLOSED) {
                ksprintf(msg, "Session DS terminee: %s",
                         event.detail[0] ? event.detail : games[sel].title);
                home_notice_set(msg);
                home_draw_ui(games, ngames, sel);
                gfx_blit_indexed_512x256(g_home_screen);
                return 0;
            } else if (event.kind == NDS_BRIDGE_EVENT_ERROR) {
                ksprintf(msg, "Bridge DS: %s",
                         event.detail[0] ? event.detail : "erreur hote");
                home_notice_set(msg);
                home_draw_ui(games, ngames, sel);
                gfx_blit_indexed_512x256(g_home_screen);
                return 0;
            }
        }

        if (!started && (timer_ms() - launch_ms) > 5000U) {
            home_notice_set("Bridge DS: delai depasse pendant l'ouverture.");
            home_draw_ui(games, ngames, sel);
            gfx_blit_indexed_512x256(g_home_screen);
            return 0;
        }

        while (keyboard_available()) {
            uint8_t key = keyboard_getkey();
            if (key == KEY_F1 || key == KEY_ESCAPE) {
                home_notice_set("Session DS en cours: ferme la fenetre externe pour revenir.");
                home_draw_bottom_panel(games, sel, sel / HOME_PAGE_SLOTS, (ngames + HOME_PAGE_SLOTS - 1) / HOME_PAGE_SLOTS);
                gfx_blit_indexed_512x256_rect(g_home_screen, HOME_BOTTOM_X, HOME_BOTTOM_Y, HOME_BOTTOM_W, HOME_BOTTOM_H);
            }
        }
        __asm__ volatile("hlt");
    }
}

static void home_draw_ui(const HomeGame *games, int ngames, int sel) {
    int page = sel / HOME_PAGE_SLOTS;
    int page_start = page * HOME_PAGE_SLOTS;
    int page_count = (ngames + HOME_PAGE_SLOTS - 1) / HOME_PAGE_SLOTS;

    home_draw_background();
    home_draw_top_bar(page, page_count);

    home_fill_round_rect(HOME_GRID_PANEL_X, HOME_GRID_PANEL_Y, HOME_GRID_PANEL_W, HOME_GRID_PANEL_H, HC_PANEL);
    home_round_rect(HOME_GRID_PANEL_X, HOME_GRID_PANEL_Y, HOME_GRID_PANEL_W, HOME_GRID_PANEL_H, HC_PANEL_EDGE);
    for (int slot = 0; slot < HOME_PAGE_SLOTS; slot++) {
        int idx = page_start + slot;
        int col = slot % HOME_COLS;
        int row = slot / HOME_COLS;
        int tx = HOME_GRID_X + col * HOME_TILE_STEP_X;
        int ty = HOME_GRID_Y + row * HOME_TILE_STEP_Y;
        if (idx >= ngames) {
            home_fill_round_rect(tx, ty, HOME_TILE_W, HOME_TILE_H, HC_BG_TOP);
            home_round_rect(tx, ty, HOME_TILE_W, HOME_TILE_H, HC_TILE_EDGE);
            continue;
        }
        home_draw_tile(tx, ty, games[idx].title, idx == sel, idx, games[idx].kind, games[idx].cover_id);
    }

    home_draw_bottom_panel(games, sel, page, page_count);
}

static void home_tile_bounds(int slot, int *x, int *y, int *w, int *h) {
    int col = slot % HOME_COLS;
    int row = slot / HOME_COLS;
    if (x) *x = HOME_GRID_X + col * HOME_TILE_STEP_X;
    if (y) *y = HOME_GRID_Y + row * HOME_TILE_STEP_Y;
    if (w) *w = HOME_TILE_W + 4;
    if (h) *h = HOME_TILE_H + 4;
}

static void home_refresh_selection(const HomeGame *games, int ngames, int old_sel, int new_sel) {
    int old_page = old_sel / HOME_PAGE_SLOTS;
    int new_page = new_sel / HOME_PAGE_SLOTS;
    int page_count = (ngames + HOME_PAGE_SLOTS - 1) / HOME_PAGE_SLOTS;
    int page_start;
    int old_slot;
    int new_slot;
    int x, y, w, h;

    if (old_sel == new_sel) return;

    if (old_page != new_page) {
        home_draw_ui(games, ngames, new_sel);
        gfx_blit_indexed_512x256(g_home_screen);
        return;
    }

    page_start = new_page * HOME_PAGE_SLOTS;
    old_slot = old_sel - page_start;
    new_slot = new_sel - page_start;

    if (old_slot >= 0 && old_slot < HOME_PAGE_SLOTS) {
        home_tile_bounds(old_slot, &x, &y, &w, &h);
        home_draw_tile(x, y, games[old_sel].title, 0, old_sel, games[old_sel].kind, games[old_sel].cover_id);
        gfx_blit_indexed_512x256_rect(g_home_screen, x, y, w, h);
    }

    if (new_slot >= 0 && new_slot < HOME_PAGE_SLOTS) {
        home_tile_bounds(new_slot, &x, &y, &w, &h);
        home_draw_tile(x, y, games[new_sel].title, 1, new_sel, games[new_sel].kind, games[new_sel].cover_id);
        gfx_blit_indexed_512x256_rect(g_home_screen, x, y, w, h);
    }

    home_draw_bottom_panel(games, new_sel, new_page, page_count);
    gfx_blit_indexed_512x256_rect(g_home_screen, HOME_BOTTOM_X, HOME_BOTTOM_Y, HOME_BOTTOM_W, HOME_BOTTOM_H);
}

static int home_move_selection(int key, int sel, int ngames) {
    int n = sel;
    if (ngames <= 0) return 0;
    if (key == KEY_LEFT) {
        n = (sel <= 0) ? (ngames - 1) : (sel - 1);
    } else if (key == KEY_RIGHT) {
        n = (sel + 1 >= ngames) ? 0 : (sel + 1);
    } else if (key == KEY_UP) {
        n = sel - HOME_COLS;
        if (n < 0) {
            int c = sel % HOME_COLS;
            n = c;
            while (n + HOME_COLS < ngames) n += HOME_COLS;
        }
    } else if (key == KEY_DOWN) {
        n = sel + HOME_COLS;
        if (n >= ngames) {
            int c = sel % HOME_COLS;
            n = c;
            while (n >= ngames && n > 0) n--;
        }
    }
    if (n < 0) n = 0;
    if (n >= ngames) n = ngames - 1;
    return n;
}

static void launcher_home(void) {
    HomeGame games[HOME_MAX_GAMES];
    int ngames = collect_home_games(games, HOME_MAX_GAMES);
    int sel = 0;

    if (ngames <= 0) {
        print_warn("Aucun jeu trouve dans vm_store.");
        tty_write("Astuce: depose des ROMs dans rom_library/ ou gba_library/ puis rebuild.");
        return;
    }

    gfx_set_mode13();
    home_setup_palette();
    home_notice_clear();
    keyboard_clear_buffer();
    home_draw_ui(games, ngames, sel);
    gfx_blit_indexed_512x256(g_home_screen);

    for (;;) {
        uint8_t key;

        key = keyboard_getkey();
        if (key == KEY_ESCAPE || key == KEY_F1) {
            gfx_restore_text_mode();
            vga_init();
            tty_init();
            tty_write("Mode terminal ALOS (F1 pour revenir au menu).");
            return;
        }
        if (key == KEY_F2 && games[sel].kind == HOME_KIND_NDS) {
            if (home_show_nds_preflight(games, ngames, sel)) return;
            continue;
        }
        if (key == KEY_ENTER) {
            if (!home_kind_actionable_in_home(games[sel].kind)) {
                char note[80];
                ksprintf(note, "%s: backend pas encore integre", home_kind_name(games[sel].kind));
                home_notice_set(note);
                home_draw_bottom_panel(games, sel, sel / HOME_PAGE_SLOTS, (ngames + HOME_PAGE_SLOTS - 1) / HOME_PAGE_SLOTS);
                gfx_blit_indexed_512x256_rect(g_home_screen, HOME_BOTTOM_X, HOME_BOTTOM_Y, HOME_BOTTOM_W, HOME_BOTTOM_H);
                continue;
            }
            if (games[sel].kind == HOME_KIND_NDS && !nds_backend_internal_ready()) {
                if (home_run_nds_session(games, ngames, sel)) return;
                continue;
            }
            char game[VM_NAME_MAX];
            kstrncpy(game, games[sel].launch, VM_NAME_MAX - 1);
            game[VM_NAME_MAX - 1] = '\0';
            gfx_restore_text_mode();
            vga_init();
            tty_init();
            launch_transparent_target(game, 0);
            gfx_set_mode13();
            home_setup_palette();
            home_notice_clear();
            home_draw_ui(games, ngames, sel);
            gfx_blit_indexed_512x256(g_home_screen);
            continue;
        }
        {
            int old_sel = sel;
            sel = home_move_selection((int)key, sel, ngames);
            home_notice_clear();
            home_refresh_selection(games, ngames, old_sel, sel);
        }
    }
}

static void jack_launch_single(const char *target) {
    int old_argc = argc;
    char *old_argv0 = argv[0];
    char *old_argv1 = argv[1];
    static char a1[RAMFS_MAX_PATH];

    kstrncpy(a1, target, RAMFS_MAX_PATH - 1);
    a1[RAMFS_MAX_PATH - 1] = '\0';

    argc = 2;
    argv[0] = (char*)"jack";
    argv[1] = a1;
    cmd_jack();

    argc = old_argc;
    argv[0] = old_argv0;
    argv[1] = old_argv1;
}

static int launch_gba_compat_if_known(const char *target_name) {
    uint32_t sz = 0;
    if (!target_name || !*target_name) return 0;

    /* Compat court terme: ROM PokemonRouge -> port VM jouable jackpokemonV */
    if (kstrcmp(target_name, "PokemonRouge.gba") == 0 ||
        kstrcmp(target_name, "PokemonRouge") == 0) {
        if (vm_store_find("jackpokemonV.vmdir", &sz)) {
            print_info("Compat GBA: lancement du port VM 'jackpokemonV'.");
            jack_launch_single("jackpokemonV");
            return 1;
        }
    }
    return 0;
}

/* Retour:
 * 0 = non reconnu
 * 1 = reconnu et traité
 */
static int launch_transparent_target(const char *target, int quiet_if_unknown) {
    char path[RAMFS_MAX_PATH];
    static const char *rom_exts[] = { ".gb", ".gbc", ".gba", ".nds", ".3ds", ".cia", 0 };
    uint32_t sz = 0;
    char vmdir[VM_NAME_MAX];

    if (!target || !*target) return 0;

    /* Jeux VM explicites */
    if (path_has_ext(target, ".vm") || path_has_ext(target, ".vmdir")) {
        jack_launch_single(target);
        return 1;
    }

    if (path_has_ext(target, ".gb") || path_has_ext(target, ".gbc") ||
        path_has_ext(target, ".gba") || path_has_ext(target, ".nds") || path_has_ext(target, ".3ds") ||
        path_has_ext(target, ".cia")) {
        uint8_t kind = home_kind_from_name(target);
        const char *base = path_basename(target);
        const char *rom = 0;
        uint32_t rom_sz = 0;
        uint32_t bios_sz = 0;
        char m[180];

        rom = vm_store_find(target, &rom_sz);
        if (!rom) {
            if (resolve_user_path(target, path) == RAMFS_OK) {
                RamFSNode *n = ramfs_find(path);
                if (n && !ramfs_is_dir(n)) {
                    rom = ramfs_data(n);
                    rom_sz = n->size;
                }
            }
        }

        if (!rom || rom_sz == 0) {
            if (!quiet_if_unknown) print_err("open: ROM introuvable");
            return 1;
        }

        if (kind == HOME_KIND_GB || kind == HOME_KIND_GBC || kind == HOME_KIND_GBA) {
            int bios_ok = shell_has_mgba_bios(base, &bios_sz);
            char emu_err[128];

            if (kind == HOME_KIND_GBA && g_gba_compat_request && launch_gba_compat_if_known(base)) {
                return 1;
            }

            ksprintf(m, "%s natif: lancement %s (%u bytes, BIOS:%s)",
                     home_kind_name(kind), base, (unsigned)rom_sz, bios_ok ? "on" : "off");
            print_info(m);

            if (gba_mgba_run_rom(base, rom, rom_sz, emu_err, sizeof(emu_err)) == 0) {
                return 1;
            }

            ksprintf(m, "%s natif: echec (%s)",
                     home_kind_name(kind), emu_err[0] ? emu_err : "inconnu");
            print_err(m);
            if (kind == HOME_KIND_GBA && g_gba_compat_request && launch_gba_compat_if_known(base)) {
                print_warn("Fallback compat VM active.");
            }
            return 1;
        }
        if (kind == HOME_KIND_NDS) {
            char nds_err[180];
            ksprintf(m, "%s: preparation %s (%u bytes)",
                     home_kind_name(kind), base, (unsigned)rom_sz);
            print_info(m);
            if (nds_backend_internal_ready()) {
                print_info("Nintendo DS: tentative de lancement via core interne.");
                home_notice_set("Nintendo DS: lancement du core interne...");
            } else if (nds_backend_bridge_ready()) {
                print_warn("Nintendo DS: mode dev host actif, pas encore portable ISO/VirtualBox.");
                home_notice_set("Nintendo DS: bridge dev externe actif.");
            } else {
                print_warn("Nintendo DS: core interne absent, preview seulement pour l'instant.");
                home_notice_set("Nintendo DS: core interne absent dans ce build.");
                return 1;
            }
            if (nds_backend_wait_session(base, rom, rom_sz, nds_err, sizeof(nds_err)) == 0) {
                print_ok(nds_err);
                home_notice_set(nds_err);
                return 1;
            }
            print_warn(nds_err);
            home_notice_set(nds_err);
            return 1;
        }

        ksprintf(m, "%s detecte: %s (%u bytes)",
                 home_kind_name(kind), base, (unsigned)rom_sz);
        print_info(m);
        ksprintf(m, "%s: backend non integre dans ALOS pour l'instant.",
                 home_kind_name(kind));
        print_warn(m);
        return 1;
    }

    /* EXE: backend reserve (future compat PE/Win32) */
    if (path_has_ext(target, ".exe")) {
        if (resolve_user_path(target, path) != RAMFS_OK || !ramfs_find(path)) {
            if (!quiet_if_unknown) print_err("open: EXE introuvable");
            return 1;
        }
        print_warn("EXE: backend non integre pour l'instant (loader PE + API Win32 requis).");
        return 1;
    }

    /* Nom court de jeu: jack <nom> => cherche <nom>.vmdir */
    ksprintf(vmdir, "%s.vmdir", target);
    if (vm_store_find(vmdir, &sz)) {
        jack_launch_single(target);
        return 1;
    }

    /* Nom court de ROM: open <nom> => cherche <nom>.<ext> */
    for (int i = 0; rom_exts[i]; i++) {
        char rom_name[VM_NAME_MAX];
        ksprintf(rom_name, "%s%s", target, rom_exts[i]);
        if (vm_store_find(rom_name, &sz)) {
            return launch_transparent_target(rom_name, quiet_if_unknown);
        }
        if (resolve_user_path(rom_name, path) == RAMFS_OK && ramfs_find(path)) {
            return launch_transparent_target(path, quiet_if_unknown);
        }
    }

    /* Fichier .vm en RamFS avec chemin relatif/absolu */
    if (resolve_user_path(target, path) == RAMFS_OK) {
        RamFSNode *n = ramfs_find(path);
        if (n && !ramfs_is_dir(n) && path_has_ext(path, ".vm")) {
            jack_launch_single(path);
            return 1;
        }
    }

    return 0;
}

static void default_vm_output_path(const char *in_path, char *out, uint32_t outsz) {
    uint32_t l = 0;
    int last_dot = -1;
    int last_slash = -1;
    for (uint32_t i = 0; in_path[i]; i++) {
        if (in_path[i] == '/') last_slash = (int)i;
        if (in_path[i] == '.') last_dot = (int)i;
    }
    while (in_path[l] && l + 1 < outsz) {
        out[l] = in_path[l];
        l++;
    }
    out[l] = '\0';
    if (last_dot > last_slash && last_dot >= 0) {
        out[last_dot] = '\0';
        l = (uint32_t)last_dot;
    }
    if (l + 4 < outsz) {
        out[l++] = '.';
        out[l++] = 'v';
        out[l++] = 'm';
        out[l] = '\0';
    }
}

static const char *resolve_rom_blob_for_shell(const char *target,
                                              char *resolved_name,
                                              uint32_t resolved_name_sz,
                                              uint32_t *out_size,
                                              uint8_t *out_kind) {
    char path[RAMFS_MAX_PATH];
    static const char *rom_exts[] = { ".gb", ".gbc", ".gba", ".nds", ".3ds", ".cia", 0 };
    RamFSNode *n = 0;
    const char *data = 0;
    uint32_t size = 0;
    uint8_t kind = 0xFF;

    if (!target || !*target) return 0;

    kind = home_kind_from_name(target);
    if (kind != 0xFF) {
        data = vm_store_find(target, &size);
        if (data) {
            if (resolved_name && resolved_name_sz) {
                kstrncpy(resolved_name, path_basename(target), resolved_name_sz - 1);
                resolved_name[resolved_name_sz - 1] = '\0';
            }
            if (out_size) *out_size = size;
            if (out_kind) *out_kind = kind;
            return data;
        }
        if (resolve_user_path(target, path) == RAMFS_OK) {
            n = ramfs_find(path);
            if (n && !ramfs_is_dir(n)) {
                if (resolved_name && resolved_name_sz) {
                    kstrncpy(resolved_name, path_basename(path), resolved_name_sz - 1);
                    resolved_name[resolved_name_sz - 1] = '\0';
                }
                if (out_size) *out_size = n->size;
                if (out_kind) *out_kind = kind;
                return ramfs_data(n);
            }
        }
        return 0;
    }

    for (int i = 0; rom_exts[i]; i++) {
        char rom_name[VM_NAME_MAX];
        ksprintf(rom_name, "%s%s", target, rom_exts[i]);
        data = resolve_rom_blob_for_shell(rom_name, resolved_name, resolved_name_sz, &size, &kind);
        if (data) {
            if (out_size) *out_size = size;
            if (out_kind) *out_kind = kind;
            return data;
        }
    }

    return 0;
}

/* ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
 *  COMMANDES
 * ????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????? */

static void cmd_help(void) {
    print_info("=== ALOS kshell v0.5 ===");
    tty_write("Fichiers :");
    tty_write("  pwd               -- dossier courant");
    tty_write("  cd [dir]          -- changer de dossier");
    tty_write("  ls [dir]          -- liste un dossier");
    tty_write("  mkdir <dir>       -- creer dossier");
    tty_write("  touch <f>         -- creer fichier vide");
    tty_write("  cat  <f>          -- afficher un fichier");
    tty_write("  write <f> <texte> -- creer/ecraser fichier");
    tty_write("  rm   <path>       -- supprimer fichier/dossier vide");
    tty_write("  open <cible> [--compat] -- lancement transparent (.vm/.vmdir/.gb/.gbc/.gba/.nds/.3ds/.cia/.exe)");
    tty_write("  home              -- interface jeux style console portable");
    tty_write("  disks             -- detecter les disques ATA/IDE visibles");
    tty_write("  install           -- mini installateur ALOS (selection disque cible)");
    tty_write("  persist [status|flush] -- etat/sync du stockage persistant");
    tty_write("ASM x86 :");
    tty_write("  run  <f.asm>      -- assembler + executer");
    tty_write("  asm  [f.asm]      -- editeur ASM interactif (. pour finir)");
    tty_write("Jack :");
    tty_write("  jackedit [f.jack] -- editeur Jack interactif (. pour finir)");
    tty_write("  jackc <in.jack> [out.vm] -- compiler Jack -> VM");
    tty_write("Hack VM (Nand2Tetris) :");
    tty_write("  jack <f.vm>...    -- charger et executer fichiers .vm");
    tty_write("  vmls              -- lister les .vm dans le RamFS");
    tty_write("  rominfo <jeu.rom> -- inspecter une ROM GB/GBC/GBA/NDS embarquee");
    tty_write("  dsdiag [jeu.nds]  -- etat DS global ou preflight d'une ROM");
    tty_write("  ndsprobe <jeu.nds> [frames] -- probe melonDS interne");
    tty_write("Audio :");
    tty_write("  audio [status|test|boot|on|off] -- controle audio systeme");
    tty_write("  beep [freq] [ms] -- bip simple via le haut-parleur PC");
    tty_write("Processus / systeme :");
    tty_write("  ps  kill <pid>  free  heap  uname  uptime  clear  echo  newtask");
    tty_write("Raccourci global :");
    tty_write("  F1               -- bascule terminal <-> menu graphique");
}

static void cmd_open(void) {
    int use_compat = 0;
    int handled;
    if (argc < 2) {
        print_err("usage: open <cible> [--compat]");
        tty_write("exemples: open Pokemon   | open Main.vm   | open PokemonRouge.gba");
        tty_write("          open PokemonBleu.gb | open PokemonDiamant.nds");
        tty_write("          open PokemonRouge.gba --compat");
        return;
    }
    if (argc >= 3) {
        if (kstrcmp(argv[2], "--compat") == 0) {
            use_compat = 1;
        } else {
            print_err("open: option inconnue");
            tty_write("usage: open <cible> [--compat]");
            return;
        }
    }
    g_gba_compat_request = use_compat;
    handled = launch_transparent_target(argv[1], 0);
    g_gba_compat_request = 0;
    if (!handled) {
        print_err("open: format inconnu ou cible introuvable");
        return;
    }
    if (path_has_ext(argv[1], ".gba") && !use_compat) {
        tty_write("Astuce: '--compat' force le fallback vers un port VM si disponible.");
    }
}

static void cmd_home(void) {
    launcher_home();
}

static void cmd_disks(void) {
    installer_list_disks();
}

static void cmd_install(void) {
    installer_run_ui();
}

static void cmd_dsdiag(void) {
    uint32_t bios7 = 0, bios9 = 0, fw = 0, keycfg = 0;
    char line[160];

    if (argc < 2) {
        int ready = nds_backend_probe_assets(&bios7, &bios9, &fw, &keycfg);
        print_info("Diagnostic Nintendo DS :");
        ksprintf(line, "  BIOS7    : %s (%u)", bios7 ? "present" : "absent", (unsigned)bios7);
        tty_write(line);
        ksprintf(line, "  BIOS9    : %s (%u)", bios9 ? "present" : "absent", (unsigned)bios9);
        tty_write(line);
        ksprintf(line, "  Firmware : %s (%u)", fw ? "present" : "absent", (unsigned)fw);
        tty_write(line);
        ksprintf(line, "  key.cfg  : %s (%u)", keycfg ? "present" : "absent", (unsigned)keycfg);
        tty_write(line);
#ifdef ALOS_HAS_MELONDS_CORE_BUILD
        tty_write("  Core ext : melonDS minimal construit dans third_party/");
#else
        tty_write("  Core ext : non construit (make melonds-core)");
#endif
        tty_write(nds_backend_internal_ready()
                      ? "  Core int : actif (probe interne disponible)"
                      : "  Core int : stub (utilise make run-nds-internal)");
        tty_write(nds_backend_bridge_ready()
                      ? "  Bridge   : host bridge actif (dev uniquement)"
                      : "  Bridge   : inactif");
        tty_write(ready ? "  Etat     : assets DS prets"
                        : "  Etat     : preflight incomplet");
        tty_write("  Usage    : dsdiag <rom.nds>");
        tty_write("  Probe    : ndsprobe <rom.nds> [frames]");
        return;
    }

    {
        char resolved[VM_NAME_MAX];
        const char *data = 0;
        uint32_t size = 0;
        uint8_t kind = 0xFF;
        char msg[176];
        NdsBackendStatus status;

        data = resolve_rom_blob_for_shell(argv[1], resolved, sizeof(resolved), &size, &kind);
        if (!data || kind != HOME_KIND_NDS) {
            print_err("dsdiag: ROM NDS introuvable");
            return;
        }

        nds_backend_status_init(&status);
        nds_backend_preflight(resolved, data, size, &status, msg, sizeof(msg));

        ksprintf(line, "ROM DS: %s", resolved);
        print_info(line);
        ksprintf(line, "  Titre    : %s", status.rom_valid && status.rom.title[0] ? status.rom.title : "(invalide)");
        tty_write(line);
        if (status.rom_valid) {
            ksprintf(line, "  Code     : %s  | Maker: %s",
                     status.rom.game_code[0] ? status.rom.game_code : "----",
                     status.rom.maker_code[0] ? status.rom.maker_code : "--");
            tty_write(line);
            ksprintf(line, "  ARM9     : off=%u entry=%08x size=%u",
                     (unsigned)status.rom.arm9_rom_offset,
                     (unsigned)status.rom.arm9_entry,
                     (unsigned)status.rom.arm9_size);
            tty_write(line);
            ksprintf(line, "  ARM7     : off=%u entry=%08x size=%u",
                     (unsigned)status.rom.arm7_rom_offset,
                     (unsigned)status.rom.arm7_entry,
                     (unsigned)status.rom.arm7_size);
            tty_write(line);
        }
        ksprintf(line, "  BIOS7    : %s (%u)", status.bios7_size ? "present" : "absent", (unsigned)status.bios7_size);
        tty_write(line);
        ksprintf(line, "  BIOS9    : %s (%u)", status.bios9_size ? "present" : "absent", (unsigned)status.bios9_size);
        tty_write(line);
        ksprintf(line, "  Firmware : %s (%u)", status.firmware_size ? "present" : "absent", (unsigned)status.firmware_size);
        tty_write(line);
        ksprintf(line, "  key.cfg  : %s (%u)", status.keycfg_size ? "present" : "absent", (unsigned)status.keycfg_size);
        tty_write(line);
#ifdef ALOS_HAS_MELONDS_CORE_BUILD
        tty_write("  Core ext : melonDS minimal construit dans third_party/");
#else
        tty_write("  Core ext : non construit (make melonds-core)");
#endif
        tty_write(nds_backend_internal_ready()
                      ? "  Core int : actif"
                      : "  Core int : absent (integration interne a faire)");
        tty_write(nds_backend_bridge_ready()
                      ? "  Bridge   : host bridge actif"
                      : "  Bridge   : aucun bridge hote");
        ksprintf(line, "  Etat     : %s", msg);
        tty_write(line);
    }
}

static void cmd_ndsprobe(void) {
    char resolved[VM_NAME_MAX];
    const char *data = 0;
    uint32_t size = 0;
    uint8_t kind = 0xFF;
    uint32_t frames = 12;
    char line[160];
    char msg[192];

    if (argc < 2) {
        print_err("usage: ndsprobe <rom.nds> [frames]");
        tty_write("exemple: ndsprobe PokemonPlatine.nds 24");
        return;
    }
    if (argc >= 3) {
        int parsed = katoi(argv[2]);
        if (parsed <= 0) {
            print_err("ndsprobe: frames doit etre > 0");
            return;
        }
        frames = (uint32_t)parsed;
        if (frames > 240u) frames = 240u;
    }

    data = resolve_rom_blob_for_shell(argv[1], resolved, sizeof(resolved), &size, &kind);
    if (!data || kind != HOME_KIND_NDS) {
        print_err("ndsprobe: ROM NDS introuvable");
        return;
    }

    ksprintf(line, "NDS probe interne: %s (%u frames)", resolved, (unsigned)frames);
    print_info(line);
    if (nds_backend_run_probe(resolved, data, size, frames, msg, sizeof(msg)) == 0) {
        print_ok(msg);
        home_notice_set(msg);
        return;
    }
    print_warn(msg);
    home_notice_set(msg);
}

static void cmd_rominfo(void) {
    char resolved[VM_NAME_MAX];
    char title_no_ext[VM_NAME_MAX];
    char cover_name[VM_NAME_MAX];
    const char *data = 0;
    uint32_t size = 0;
    uint32_t bios_sz = 0;
    uint32_t csz = 0;
    uint8_t kind = 0xFF;
    char size_buf[16];
    GbaRomInfo info;

    if (argc < 2) {
        print_err("usage: rominfo <rom.gb|rom.gbc|rom.gba|rom.nds>");
        return;
    }

    data = resolve_rom_blob_for_shell(argv[1], resolved, sizeof(resolved), &size, &kind);
    if (!data || size == 0) {
        print_err("rominfo: ROM introuvable");
        return;
    }
    home_format_size(size_buf, sizeof(size_buf), size);

    kstrncpy(title_no_ext, resolved, sizeof(title_no_ext) - 1);
    title_no_ext[sizeof(title_no_ext) - 1] = '\0';
    {
        const char *ext = path_ext(title_no_ext);
        if (ext) title_no_ext[ext - title_no_ext] = '\0';
    }
    ksprintf(cover_name, "%s/cover.png", title_no_ext);

    {
        char line[128];
        ksprintf(line, "ROM: %s", resolved);
        print_info(line);
        ksprintf(line, "  Plateforme : %s", home_kind_name(kind));
        tty_write(line);
        ksprintf(line, "  Taille     : %s", size_buf);
        tty_write(line);
        ksprintf(line, "  Cover  : %s", vm_store_find(cover_name, &csz) ? "present" : "absent");
        tty_write(line);
        if (kind == HOME_KIND_GBA) {
            gba_rom_info_init(&info);
            if (!gba_rom_info_parse(data, size, &info)) {
                print_err("rominfo: en-tete GBA invalide");
                return;
            }
            ksprintf(line, "  Header     : %s", info.header_title[0] ? info.header_title : "(vide)");
            tty_write(line);
            ksprintf(line, "  Code       : %s  | Maker: %s  | Ver: %u",
                     info.game_code[0] ? info.game_code : "----",
                     info.maker_code[0] ? info.maker_code : "--",
                     (unsigned)info.version);
            tty_write(line);
            ksprintf(line, "  Save       : %s", info.save_desc);
            tty_write(line);
            ksprintf(line, "  Hardware   : %s", info.hw_desc);
            tty_write(line);
            ksprintf(line, "  BIOS       : %s", shell_has_mgba_bios(resolved, &bios_sz) ? "present" : "absent");
            tty_write(line);
        } else {
            ksprintf(line, "  Backend    : %s",
                     kind == HOME_KIND_NDS ? (nds_backend_internal_ready()
                                                   ? "interne video"
                                                   : (nds_backend_bridge_ready()
                                                          ? "bridge dev"
                                                          : "preview"))
                                           : (home_kind_supported(kind) ? "pret" : "a integrer"));
            tty_write(line);
            if (kind == HOME_KIND_GB || kind == HOME_KIND_GBC) {
                ksprintf(line, "  BIOS       : %s", shell_has_mgba_bios(resolved, &bios_sz) ? "present" : "absent");
                tty_write(line);
            }
            if (kind == HOME_KIND_NDS) {
                NdsRomInfo nds;
                uint32_t bios7 = 0, bios9 = 0, fw = 0, keycfg = 0;
                nds_rom_info_init(&nds);
                if (!nds_rom_info_parse(data, size, &nds)) {
                    print_err("rominfo: header NDS invalide");
                    return;
                }
                ksprintf(line, "  Titre      : %s", nds.title[0] ? nds.title : "(vide)");
                tty_write(line);
                ksprintf(line, "  Code       : %s  | Maker: %s",
                         nds.game_code[0] ? nds.game_code : "----",
                         nds.maker_code[0] ? nds.maker_code : "--");
                tty_write(line);
                ksprintf(line, "  ARM9       : off=%u entry=%08x size=%u",
                         (unsigned)nds.arm9_rom_offset,
                         (unsigned)nds.arm9_entry,
                         (unsigned)nds.arm9_size);
                tty_write(line);
                ksprintf(line, "  ARM7       : off=%u entry=%08x size=%u",
                         (unsigned)nds.arm7_rom_offset,
                         (unsigned)nds.arm7_entry,
                         (unsigned)nds.arm7_size);
                tty_write(line);
                nds_backend_probe_assets(&bios7, &bios9, &fw, &keycfg);
                ksprintf(line, "  BIOS7      : %s (%u)", bios7 ? "present" : "absent", (unsigned)bios7);
                tty_write(line);
                ksprintf(line, "  BIOS9      : %s (%u)", bios9 ? "present" : "absent", (unsigned)bios9);
                tty_write(line);
                ksprintf(line, "  Firmware   : %s (%u)", fw ? "present" : "absent", (unsigned)fw);
                tty_write(line);
                ksprintf(line, "  key.cfg    : %s (%u)", keycfg ? "present" : "absent", (unsigned)keycfg);
                tty_write(line);
                ksprintf(line, "  Core int   : %s", nds_backend_internal_ready() ? "actif" : "absent");
                tty_write(line);
                ksprintf(line, "  Bridge     : %s", nds_backend_bridge_ready() ? "host actif (dev)" : "host inactif");
                tty_write(line);
            }
        }
    }
}

static void cmd_persist(void) {
    if (argc == 1 || (argc >= 2 && kstrcmp(argv[1], "status") == 0)) {
        if (persist_is_enabled()) print_ok("Persist: active");
        else print_warn("Persist: inactif (partition disque type 0xA0 absente)");
        return;
    }
    if (argc >= 2 && kstrcmp(argv[1], "flush") == 0) {
        persist_flush();
        if (persist_is_enabled()) print_ok("Persist: flush effectue");
        else print_warn("Persist: inactif");
        return;
    }
    print_err("usage: persist [status|flush]");
}

static void cmd_ls(void) {
    char path[RAMFS_MAX_PATH];
    char buf[2048];
    if (argc >= 2) {
        if (resolve_user_path(argv[1], path) != RAMFS_OK) { print_err("ls: chemin invalide"); return; }
    } else {
        kstrncpy(path, g_cwd, RAMFS_MAX_PATH - 1);
        path[RAMFS_MAX_PATH - 1] = '\0';
    }

    if (ramfs_list_dir(path, buf, sizeof(buf)) < 0) {
        print_err("ls: dossier introuvable");
        return;
    }

    {
        char hdr[120];
        ksprintf(hdr, "Listing: %s", path);
        print_info(hdr);
    }

    char line[128]; int i=0; char *p=buf;
    while(*p){
        if(*p=='\n'){ line[i]='\0'; if(i>0){char o[140];ksprintf(o,"  %s",line);tty_write(o);} i=0; }
        else if(i<127) line[i++]=*p;
        p++;
    }
    if(i){line[i]='\0';char o[140];ksprintf(o,"  %s",line);tty_write(o);}
}

static void cmd_cat(void) {
    char path[RAMFS_MAX_PATH];
    if(argc<2){print_err("usage: cat <f>");return;}
    if (resolve_user_path(argv[1], path) != RAMFS_OK) { print_err("cat: chemin invalide"); return; }
    RamFSNode *n=ramfs_find(path);
    if(!n){print_err("cat: introuvable");return;}
    if (ramfs_is_dir(n)) { print_err("cat: est un dossier"); return; }
    char line[82]; int li=0;
    for(uint32_t i=0;i<=n->size;i++){
        const char *_nd=ramfs_data(n); char c=(i<n->size)?_nd[i]:'\n';
        if(c=='\n'){line[li]='\0';tty_write(line);li=0;}
        else if(li<79) line[li++]=c;
    }
}

static void cmd_write(void) {
    char path[RAMFS_MAX_PATH];
    if(argc<3){print_err("usage: write <f> <texte>");return;}
    for(int i=3;i<argc;i++) *(argv[i]-1)=' ';
    if (resolve_user_path(argv[1], path) != RAMFS_OK) { print_err("write: chemin invalide"); return; }
    const char *content=argv[2];
    RamFSNode *n=ramfs_find(path);
    if(n){
        if (ramfs_is_dir(n)) { print_err("write: est un dossier"); return; }
        if (is_read_only_node(n)) { print_err("write: fichier read-only"); return; }
        uint32_t len=kstrlen(content); if(len>=RAMFS_MAX_SIZE)len=RAMFS_MAX_SIZE-1;
        kmemcpy(n->data,content,len); n->data[len]='\0'; n->size=len;
        persist_mark_dirty();
        print_ok("Fichier mis a jour.");
    } else {
        if(!ramfs_create(path,content)) print_err("write: creation impossible");
        else print_ok("Fichier cree.");
    }
}

static void cmd_rm(void) {
    char path[RAMFS_MAX_PATH];
    int rc;
    if(argc<2){print_err("usage: rm <path>");return;}
    if (resolve_user_path(argv[1], path) != RAMFS_OK) { print_err("rm: chemin invalide"); return; }
    rc = ramfs_remove(path);
    if (rc == RAMFS_OK) { print_ok("Supprime."); return; }
    if (rc == RAMFS_ERR_NOTFOUND) { print_err("rm: introuvable"); return; }
    if (rc == RAMFS_ERR_RO) { print_err("rm: read-only"); return; }
    if (rc == RAMFS_ERR_DIR_NOTEMPTY) { print_err("rm: dossier non vide"); return; }
    print_err("rm: echec");
}

static void cmd_run(void) {
    char path[RAMFS_MAX_PATH];
    if(argc<2){print_err("usage: run <f.asm>");return;}
    if (resolve_user_path(argv[1], path) != RAMFS_OK) { print_err("run: chemin invalide"); return; }
    RamFSNode *n=ramfs_find(path);
    if(!n){print_err("run: introuvable");return;}
    if (ramfs_is_dir(n)) { print_err("run: est un dossier"); return; }
    char taskname[TASK_NAME_LEN];
    {
        const char *base = path_basename(path);
        kstrncpy(taskname, base, TASK_NAME_LEN - 1);
        taskname[TASK_NAME_LEN - 1] = '\0';
        const char *dot=kstrchr(taskname,'.'); if(dot) taskname[(uint32_t)(dot-taskname)]='\0';
    }
    print_info("Assemblage...");
    char errbuf[128];
    int rc=asmexec_run(taskname,ramfs_data(n),errbuf,sizeof(errbuf));
    char msg[160];
    switch(rc){
        case ASMEXEC_OK: ksprintf(msg,"[OK] Tache '%s' lancee.",taskname); print_ok(msg); break;
        case ASMEXEC_ERR_SYNTAX: ksprintf(msg,"[ERR] Syntaxe: %s",errbuf); print_err(msg); break;
        case ASMEXEC_ERR_UNDEF:  ksprintf(msg,"[ERR] Label: %s",errbuf);   print_err(msg); break;
        case ASMEXEC_ERR_OOM:    print_err("[ERR] Memoire insuffisante.");  break;
        default:                 print_err("[ERR] Trop grand.");            break;
    }
}

static void cmd_asm_editor(void) {
    char fullpath[RAMFS_MAX_PATH];
    const char *fname=(argc>=2)?argv[1]:"prog.asm";
    char buf[RAMFS_MAX_SIZE]; int bpos=0;
    if (resolve_user_path(fname, fullpath) != RAMFS_OK) { print_err("asm: chemin invalide"); return; }
    char hdr[110]; ksprintf(hdr,"-- Editeur ASM '%s' (terminer avec '.') --",fullpath);
    print_info(hdr);
    while(1){
        tty_clear_input(); vga_print_at(0,24,"... ",0x0E);
        int kcx=4; char kline[80]; int klen=0;
        while(1){
            uint8_t key=keyboard_getkey();
            if(key==KEY_ENTER) break;
            if(key==KEY_BACKSPACE&&klen>0){klen--;kcx--;tty_backspace(kcx);continue;}
            if(key>=0x20&&key<0x80&&klen<78){kline[klen++]=(char)key;tty_echo_char((char)key,kcx++);}
        }
        kline[klen]='\0';
        if(klen==1&&kline[0]=='.') break;
        char echo[84]; ksprintf(echo,"    %s",kline); tty_write_color(echo,TTY_C_MUTED);
        int rem=(int)RAMFS_MAX_SIZE-bpos-2;
        if(rem<klen+1){print_err("Buffer plein.");break;}
        kmemcpy(buf+bpos,kline,(uint32_t)klen); bpos+=klen; buf[bpos++]='\n';
    }
    buf[bpos]='\0'; tty_clear_input();
    if(bpos==0){print_warn("Vide, annule.");return;}
    RamFSNode *node=ramfs_find(fullpath);
    if(node){
        if (ramfs_is_dir(node) || is_read_only_node(node)) { print_err("asm: cible non modifiable"); return; }
        kmemcpy(node->data,buf,(uint32_t)(bpos+1));node->size=(uint32_t)bpos;
        persist_mark_dirty();
    }
    else{ node=ramfs_create(fullpath,buf); if(!node){print_err("RamFS plein.");return;} }
    print_ok("Sauvegarde.");
    char taskname[TASK_NAME_LEN]; kstrncpy(taskname,path_basename(fullpath),TASK_NAME_LEN - 1);
    taskname[TASK_NAME_LEN - 1] = '\0';
    const char *dot=kstrchr(taskname,'.'); if(dot) taskname[(uint32_t)(dot-taskname)]='\0';
    char errbuf[128]; int rc=asmexec_run(taskname,buf,errbuf,sizeof(errbuf));
    char msg[160];
    if(rc==ASMEXEC_OK){ksprintf(msg,"[OK] Tache '%s' lancee.",taskname);print_ok(msg);}
    else{ksprintf(msg,"[ERR] %s",errbuf);print_err(msg);}
}

static void cmd_jack_editor(void) {
    char fullpath[RAMFS_MAX_PATH];
    const char *fname = (argc >= 2) ? argv[1] : "Main.jack";
    char buf[RAMFS_MAX_SIZE];
    int bpos = 0;

    if (resolve_user_path(fname, fullpath) != RAMFS_OK) { print_err("jackedit: chemin invalide"); return; }
    {
        char hdr[112];
        ksprintf(hdr, "-- Editeur Jack '%s' (terminer avec '.') --", fullpath);
        print_info(hdr);
    }

    while (1) {
        tty_clear_input();
        vga_print_at(0,24,"... ",0x0E);
        int kcx = 4;
        char kline[80];
        int klen = 0;
        while (1) {
            uint8_t key = keyboard_getkey();
            if (key == KEY_ENTER) break;
            if (key == KEY_BACKSPACE && klen > 0) { klen--; kcx--; tty_backspace(kcx); continue; }
            if (key >= 0x20 && key < 0x80 && klen < 78) { kline[klen++] = (char)key; tty_echo_char((char)key, kcx++); }
        }
        kline[klen] = '\0';
        if (klen == 1 && kline[0] == '.') break;

        {
            char echo[84];
            ksprintf(echo, "    %s", kline);
            tty_write_color(echo, TTY_C_MUTED);
        }

        if (bpos + klen + 2 >= (int)RAMFS_MAX_SIZE) { print_err("jackedit: buffer plein."); break; }
        kmemcpy(buf + bpos, kline, (uint32_t)klen);
        bpos += klen;
        buf[bpos++] = '\n';
    }
    buf[bpos] = '\0';
    tty_clear_input();

    if (bpos == 0) { print_warn("Vide, annule."); return; }

    {
        RamFSNode *node = ramfs_find(fullpath);
        if (node) {
            if (ramfs_is_dir(node) || is_read_only_node(node)) { print_err("jackedit: cible non modifiable"); return; }
            kmemcpy(node->data, buf, (uint32_t)(bpos + 1));
            node->size = (uint32_t)bpos;
            persist_mark_dirty();
        } else {
            node = ramfs_create(fullpath, buf);
            if (!node) { print_err("jackedit: creation impossible"); return; }
        }
    }
    print_ok("Sauvegarde Jack OK.");
}

static void cmd_jackc(void) {
    char in_path[RAMFS_MAX_PATH];
    char out_path[RAMFS_MAX_PATH];
    char *vm_out = 0;
    char errbuf[192];
    uint32_t out_len = 0;
    int rc;

    if (argc < 2) { print_err("usage: jackc <in.jack> [out.vm]"); return; }

    if (resolve_user_path(argv[1], in_path) != RAMFS_OK) { print_err("jackc: chemin source invalide"); return; }
    if (argc >= 3) {
        if (resolve_user_path(argv[2], out_path) != RAMFS_OK) { print_err("jackc: chemin sortie invalide"); return; }
    } else {
        default_vm_output_path(in_path, out_path, sizeof(out_path));
    }

    {
        RamFSNode *src = ramfs_find(in_path);
        if (!src) { print_err("jackc: source introuvable"); return; }
        if (ramfs_is_dir(src)) { print_err("jackc: source est un dossier"); return; }

        vm_out = (char*)kmalloc(RAMFS_MAX_SIZE);
        if (!vm_out) { print_err("jackc: OOM"); return; }
        vm_out[0] = '\0';

        rc = jackc_compile(in_path, ramfs_data(src), vm_out, RAMFS_MAX_SIZE, &out_len, errbuf, sizeof(errbuf));
        if (rc < 0) {
            char m[220];
            ksprintf(m, "jackc: %s", errbuf[0] ? errbuf : "echec compilation");
            print_err(m);
            kfree(vm_out);
            return;
        }
    }

    {
        RamFSNode *dst = ramfs_find(out_path);
        if (dst) {
            if (ramfs_is_dir(dst) || is_read_only_node(dst)) {
                print_err("jackc: destination non modifiable");
                kfree(vm_out);
                return;
            }
            kmemcpy(dst->data, vm_out, out_len);
            dst->data[out_len] = '\0';
            dst->size = out_len;
            persist_mark_dirty();
        } else {
            dst = ramfs_create(out_path, vm_out);
            if (!dst) {
                print_err("jackc: creation destination impossible");
                kfree(vm_out);
                return;
            }
        }
    }

    {
        char msg[160];
        ksprintf(msg, "Jack compile OK: %s -> %s (%u bytes)", in_path, out_path, (unsigned)out_len);
        print_ok(msg);
    }
    kfree(vm_out);
}

/* ?????? vmls : liste jeux et fichiers .vm ??????????????????????????????????????????????????????????????????????????????????????????????????? */
static void cmd_vmls(void) {
    char buf[4096]; vm_store_list(buf,sizeof(buf));
    uint32_t bios_sz = 0;
    uint32_t nds_bios7 = 0, nds_bios9 = 0, nds_fw = 0, nds_keycfg = 0;
    {
        char st[96];
        ksprintf(st,"VM store: %d/%u fichiers, %u bytes",
                 vm_store_count(), vm_store_capacity(), vm_store_bytes());
        print_info(st);
    }
    if (shell_has_mgba_bios("PokemonRougeFeu.gba", &bios_sz)) {
        char bmsg[96];
        ksprintf(bmsg, "BIOS GBA: present (%u bytes)", (unsigned)bios_sz);
        print_ok(bmsg);
    } else {
        print_warn("BIOS GBA: absent (attendu: gba_bios.bin de 16384 bytes)");
    }
    if (shell_has_mgba_bios("PokemonRouge.gbc", &bios_sz)) {
        char bmsg[96];
        ksprintf(bmsg, "BIOS GB/GBC: present (%u bytes)", (unsigned)bios_sz);
        print_ok(bmsg);
    } else {
        print_warn("BIOS GB/GBC: absent (attendu: gb_bios.bin / dmg0_rom.bin / gbc_bios.bin)");
    }
    if (nds_backend_probe_assets(&nds_bios7, &nds_bios9, &nds_fw, &nds_keycfg)) {
        char bmsg[128];
        ksprintf(bmsg, "BIOS NDS: bios7=%u bios9=%u firmware=%u keycfg=%u",
                 (unsigned)nds_bios7, (unsigned)nds_bios9,
                 (unsigned)nds_fw, (unsigned)nds_keycfg);
        print_ok(bmsg);
    } else {
        print_warn("BIOS NDS: incomplet (biosnds7.rom / biosnds9.rom / firmware.bin attendus)");
    }
    /* D'abord les jeux VM (.vmdir) et ROM */
    int found_games=0;
    int found_roms=0;
    char line[128]; int li=0; char *p=buf;
    while(*p){
        if(*p=='\n'){
            line[li]='\0';
            int l=(int)kstrlen(line);
            if(l>6&&line[l-6]=='.'&&line[l-5]=='v'&&line[l-4]=='m'
               &&line[l-3]=='d'&&line[l-2]=='i'&&line[l-1]=='r'){
                if(!found_games){print_info("Jeux disponibles (jack <nom>) :"); found_games=1;}
                /* Afficher sans l'extension .vmdir */
                char gamename[64]; kstrncpy(gamename,line,(uint32_t)(l-6));
                gamename[l-6]='\0';
                char o[120]; ksprintf(o,"  jack %-20s  ??? lancer ce jeu",gamename);
                print_ok(o);
            } else if (home_kind_is_rom(home_kind_from_name(line))) {
                if(!found_roms){print_info("ROMs embarquees (open <nom>) :"); found_roms=1;}
                char o[132]; ksprintf(o,"  %s",line); tty_write(o);
            }
            li=0;
        } else if(li<127) line[li++]=*p;
        p++;
    }
    if(!found_games && !found_roms) tty_write("Aucun jeu installe. Depose des ROMs dans rom_library/ ou gba_library/ puis relance make.");
    /* Ensuite les .vm individuels */
    tty_write("");
    print_info("Fichiers .vm individuels :");
    li=0; p=buf;
    while(*p){
        if(*p=='\n'){
            line[li]='\0';
            int l=(int)kstrlen(line);
            if(l>3&&line[l-3]=='.'&&line[l-2]=='v'&&line[l-1]=='m'){
                char o[132]; ksprintf(o,"  %s",line); tty_write(o);
            }
            li=0;
        } else if(li<127) line[li++]=*p;
        p++;
    }
}

/* ?????? Helpers jack ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????? */

static void jack_launch(const char **files, int nfiles) {
    if(nfiles<=0){print_err("Aucun fichier VM a lancer.");return;}
    VMState *vm=(VMState*)kmalloc(sizeof(VMState));
    if(!vm){print_err("OOM.");return;}
    vm_init(vm);

    char errbuf[128];
    int rc=vm_load(vm,files,nfiles,errbuf,sizeof(errbuf));
    if(rc<0){
        char msg[160]; ksprintf(msg,"Erreur: %s",errbuf);
        print_err(msg); vm_destroy(vm); kfree(vm); return;
    }
    char msg[80];
    ksprintf(msg,"[OK] %d instructions, main=%d. Demarrage...",
             (int)vm->prog_len,(int)vm->main_entry);
    print_ok(msg);
    for(volatile int i=0;i<6000000;i++);

    vm_run(vm);
    vm_destroy(vm); kfree(vm);
    /* Retour robuste en mode texte meme si la VM a quitte en plein rendu. */
    gfx_restore_text_mode();
    vga_init();
    tty_init();
    tty_write("Retour au shell ALOS.");
}

/* ?????? jack : lance un jeu depuis vm_store ????????????????????????????????????????????????????????????????????????????????????????????????
 * Cas 1 : jack <nom>     ??? cherche <nom>.vmdir, collecte les .vm du jeu
 *                          directement dans vm_store par comparaison exacte
 * Cas 2 : jack <f1> ...  ??? liste explicite de .vm (cherch??s dans vm_store)
 * ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????? */
static void cmd_jack(void) {
    if (argc < 2) {
        print_err("usage: jack <jeu>  ou  jack <f1.vm> [f2.vm ...]");
        tty_write("  Tapez 'vmls' pour voir les jeux disponibles.");
        return;
    }

    /* ?????? Cas 1 : un seul argument qui n'est pas un .vm ??????????????????????????????????????????????????? */
    const char *arg1_dot = kstrchr(argv[1], '.');
    int arg1_is_vm = arg1_dot && kstrcmp(arg1_dot, ".vm") == 0;

    if (argc == 2 && !arg1_is_vm) {
        /* Construire le nom du vmdir */
        char vmdir_name[64];
        const char *arg1_ext = kstrchr(argv[1], '.');
        if (arg1_ext && kstrcmp(arg1_ext, ".vmdir") == 0)
            kstrncpy(vmdir_name, argv[1], 64);
        else
            ksprintf(vmdir_name, "%s.vmdir", argv[1]);

        /* V??rifier que le vmdir existe */
        uint32_t dir_size = 0;
        const char *dir_data = vm_store_find(vmdir_name, &dir_size);
        if (!dir_data) {
            char msg[80];
            ksprintf(msg, "Jeu '%s' introuvable. Tapez 'vmls'.", argv[1]);
            print_err(msg); return;
        }

        /* Lire le manifeste .vmdir et charger uniquement ces .vm */
        static const char *vm_files[64];
        static char stored_names[64][VM_MAX_NAME];
        int nfiles = 0;
        char fname[VM_MAX_NAME];
        int fi = 0;
        for (uint32_t i = 0; i <= dir_size; i++) {
            char c = (i < dir_size) ? dir_data[i] : '\n';
            if (c == '\r') continue;
            if (c == '\n' || c == '\0') {
                if (fi > 0) {
                    fname[fi] = '\0';
                    if (nfiles >= 64) {
                        print_err("Manifeste trop grand (max 64 fichiers).");
                        return;
                    }
                    kstrncpy(stored_names[nfiles], fname, VM_MAX_NAME);
                    {
                        uint32_t fsz = 0;
                        if (!vm_store_find(stored_names[nfiles], &fsz)) {
                            char msg[96];
                            ksprintf(msg, "VM manquante: %s", stored_names[nfiles]);
                            print_err(msg);
                            return;
                        }
                    }
                    vm_files[nfiles] = stored_names[nfiles];
                    nfiles++;
                    fi = 0;
                }
                if (c == '\0') break;
            } else if (fi < (VM_MAX_NAME - 1)) {
                fname[fi++] = c;
            }
        }

        if (nfiles == 0) {
            print_err("Manifeste vide.");
            return;
        }
        char msg[120];
        ksprintf(msg, "Jeu '%s' ??? %d fichier(s) VM", argv[1], nfiles);
        print_info(msg);
        jack_launch(vm_files, nfiles);
        return;
    }

    /* ?????? Cas 2 : liste explicite de .vm ???????????????????????????????????????????????????????????????????????????????????????????????? */
    static const char *explicit_files[64];
    static char explicit_paths[64][RAMFS_MAX_PATH];
    int nexplicit = 0;
    for (int i = 1; i < argc && nexplicit < 64; i++) {
        const char *cand = argv[i];
        if (kstrchr(cand, '/')) {
            if (resolve_user_path(cand, explicit_paths[nexplicit]) != RAMFS_OK) {
                char msg[96]; ksprintf(msg, "Chemin invalide: %s", cand);
                print_err(msg); return;
            }
            cand = explicit_paths[nexplicit];
        } else {
            if (resolve_user_path(cand, explicit_paths[nexplicit]) == RAMFS_OK) {
                /* Prefer absolute path if it exists in ramfs */
                if (ramfs_find(explicit_paths[nexplicit])) cand = explicit_paths[nexplicit];
            }
        }

        uint32_t fsz = 0;
        const char *fd = vm_store_find(cand, &fsz);
        if (!fd && !ramfs_find(cand)) {
            char msg[96]; ksprintf(msg, "Fichier introuvable: %s", cand);
            print_err(msg); return;
        }
        explicit_files[nexplicit++] = cand;
    }
    jack_launch(explicit_files, nexplicit);
}

static void cmd_ps(void){
    static const char *states[]={"READY  ","RUNNING","BLOCKED","ZOMBIE "};
    Task *t=task_table(); print_info("PID  ETAT     NOM"); char line[64];
    for(int i=0;i<MAX_TASKS;i++){
        if(!t[i].name[0]) continue;
        ksprintf(line,"  %-3u  %s  %s",t[i].pid,states[t[i].state<4?t[i].state:0],t[i].name);
        tty_write(line);
    }
}

static void cmd_kill(void){
    if(argc<2){print_err("usage: kill <pid>");return;}
    uint32_t pid=(uint32_t)katoi(argv[1]);
    if(pid==0){print_err("Impossible de tuer kshell.");return;}
    task_kill(pid); char msg[48]; ksprintf(msg,"Signal envoye pid %u.",pid); print_ok(msg);
}

static void cmd_free(void){
    uint32_t fr=pmm_free_pages(),to=pmm_total_pages(),us=pmm_used_pages(); char line[80];
    print_info("Memoire physique :");
    ksprintf(line,"  Total   : %u Ko",to*4);  tty_write(line);
    ksprintf(line,"  Utilise : %u Ko",us*4); tty_write(line);
    ksprintf(line,"  Libre   : %u Ko",fr*4);  print_ok(line);
}

static void cmd_heap(void){
    char line[96];
    uint32_t used=heap_used(), freeb=heap_free(), big=heap_largest_free();
    uint32_t blocks=heap_block_count(), free_blocks=heap_free_block_count();
    print_info("Heap kernel :");
    ksprintf(line,"  Utilise : %u o",used); tty_write(line);
    ksprintf(line,"  Libre   : %u o",freeb); tty_write(line);
    ksprintf(line,"  Plus grand bloc libre : %u o",big); tty_write(line);
    ksprintf(line,"  Blocs   : %u (libres %u)",blocks,free_blocks); tty_write(line);
    ksprintf(line,"  Total   : %u Ko",HEAP_SIZE/1024); tty_write(line);
}

static void cmd_uname(void){
    print_info("ALOS 0.5  i386");
    tty_write("  PMM | Heap | Preemptif | VFS/RamFS | INT 0x80 | ASM exec | Hack VM");
    tty_write("  Clavier PS/2 AZERTY | Audio PC speaker | Nand2Tetris Jack compatible");
}

static void cmd_uptime(void){
    char msg[64]; uint32_t ms=timer_ms();
    ksprintf(msg,"Uptime: %u s  (%u ms)",ms/1000,ms); print_info(msg);
}

static void cmd_audio(void) {
    char line[96];
    const BootInfo *bi = bootinfo_get();

    if (!audio_is_ready()) {
        print_warn("Audio: backend non initialise.");
        return;
    }

    if (argc < 2 || kstrcmp(argv[1], "status") == 0) {
        ksprintf(line, "Audio: %s | backend: PC speaker",
                 audio_is_enabled() ? "active" : "coupe");
        print_info(line);
        if (audio_has_ac97_controller()) {
            char pci_line[96];
            ksprintf(pci_line, "  PCI audio: AC97 detecte (%04x:%04x)",
                     (unsigned)audio_ac97_vendor_id(),
                     (unsigned)audio_ac97_device_id());
            tty_write(pci_line);
            tty_write("  Note: VirtualBox expose souvent l'audio via AC97.");
            tty_write("  Le backend actuel PC speaker peut donc rester muet dans cette VM.");
        } else {
            tty_write("  PCI audio: aucun AC97 detecte");
        }
        if (bi->hypervisor_present && bi->hypervisor_vendor[0]) {
            char hv_line[96];
            ksprintf(hv_line, "  Hyperviseur: %s", bi->hypervisor_vendor);
            tty_write(hv_line);
        }
        tty_write("  audio test  -- jouer une courte sequence");
        tty_write("  audio boot  -- rejouer le jingle de demarrage");
        tty_write("  audio on/off");
        return;
    }

    if (kstrcmp(argv[1], "test") == 0) {
        print_info("Audio: lecture du motif de test.");
        audio_play_test_pattern();
        return;
    }
    if (kstrcmp(argv[1], "boot") == 0) {
        print_info("Audio: lecture du jingle de boot.");
        audio_play_boot_jingle();
        return;
    }
    if (kstrcmp(argv[1], "on") == 0) {
        audio_set_enabled(1);
        print_ok("Audio active.");
        return;
    }
    if (kstrcmp(argv[1], "off") == 0) {
        audio_set_enabled(0);
        print_warn("Audio coupe.");
        return;
    }

    print_err("usage: audio [status|test|boot|on|off]");
}

static void cmd_beep(void) {
    uint32_t freq = 880;
    uint32_t duration = 120;

    if (!audio_is_ready()) {
        print_warn("beep: audio non initialise.");
        return;
    }
    if (!audio_is_enabled()) {
        print_warn("beep: audio coupe (utilise 'audio on').");
        return;
    }
    if (argc >= 2) {
        int parsed = katoi(argv[1]);
        if (parsed <= 0) {
            print_err("beep: freq invalide");
            return;
        }
        if (parsed < 20) parsed = 20;
        if (parsed > 20000) parsed = 20000;
        freq = (uint32_t)parsed;
    }
    if (argc >= 3) {
        int parsed = katoi(argv[2]);
        if (parsed <= 0) {
            print_err("beep: duree invalide");
            return;
        }
        if (parsed > 5000) parsed = 5000;
        duration = (uint32_t)parsed;
    }

    audio_beep(freq, duration);
}

static void cmd_echo(void){
    if(argc<2){tty_write("");return;}
    char out[256]; int n=0;
    for(int i=1;i<argc;i++){char *p=argv[i];while(*p&&n<254)out[n++]=*p++;if(i<argc-1&&n<254)out[n++]=' ';}
    out[n]='\0'; tty_write(out);
}

static void cmd_pwd(void){
    tty_write(g_cwd);
}

static void cmd_cd(void){
    const char *target = (argc >= 2) ? argv[1] : "/home/root";
    char path[RAMFS_MAX_PATH];
    RamFSNode *n;
    if (resolve_user_path(target, path) != RAMFS_OK) { print_err("cd: chemin invalide"); return; }
    n = ramfs_find(path);
    if (!n) { print_err("cd: introuvable"); return; }
    if (!ramfs_is_dir(n)) { print_err("cd: pas un dossier"); return; }
    kstrncpy(g_cwd, path, RAMFS_MAX_PATH - 1);
    g_cwd[RAMFS_MAX_PATH - 1] = '\0';
}

static void cmd_mkdir(void){
    char path[RAMFS_MAX_PATH];
    int rc;
    if (argc < 2) { print_err("usage: mkdir <dir>"); return; }
    if (resolve_user_path(argv[1], path) != RAMFS_OK) { print_err("mkdir: chemin invalide"); return; }
    rc = ramfs_mkdir(path);
    if (rc == RAMFS_OK) { print_ok("Dossier cree."); return; }
    if (rc == RAMFS_ERR_EXISTS) { print_err("mkdir: existe deja"); return; }
    if (rc == RAMFS_ERR_NOTFOUND || rc == RAMFS_ERR_NOTDIR) { print_err("mkdir: parent invalide"); return; }
    if (rc == RAMFS_ERR_FULL) { print_err("mkdir: RamFS pleine"); return; }
    print_err("mkdir: echec");
}

static void cmd_touch(void){
    char path[RAMFS_MAX_PATH];
    RamFSNode *n;
    if (argc < 2) { print_err("usage: touch <fichier>"); return; }
    if (resolve_user_path(argv[1], path) != RAMFS_OK) { print_err("touch: chemin invalide"); return; }
    n = ramfs_find(path);
    if (n) {
        if (ramfs_is_dir(n)) { print_err("touch: est un dossier"); return; }
        if (is_read_only_node(n)) { print_err("touch: read-only"); return; }
        print_ok("Fichier existe.");
        return;
    }
    if (!ramfs_create(path, "")) { print_err("touch: creation impossible"); return; }
    print_ok("Fichier cree.");
}

static void idle_task(void){while(1)__asm__ volatile("hlt");}
static void cmd_newtask(void){
    const char *name=argc>=2?argv[1]:"idle";
    int pid=task_create(name,idle_task);
    if(pid<0) print_err("Table pleine.");
    else{char m[48];ksprintf(m,"[OK] '%s' cree (pid %d).",name,pid);print_ok(m);}
}

#ifdef ALOS_NDS_AUTOPROBE_ROM
static void shell_run_nds_autoprobe(void) {
    char resolved[VM_NAME_MAX];
    const char *data = 0;
    uint32_t size = 0;
    uint8_t kind = 0xFF;
    char msg[192];
    char line[160];

    data = resolve_rom_blob_for_shell(ALOS_NDS_AUTOPROBE_ROM, resolved, sizeof(resolved), &size, &kind);
    if (!data || kind != HOME_KIND_NDS) {
        ksprintf(msg, "AUTO-VIDEO DS: ROM introuvable (%s)", ALOS_NDS_AUTOPROBE_ROM);
        print_warn(msg);
        home_notice_set(msg);
        return;
    }

    ksprintf(line, "AUTO-VIDEO DS: %s", resolved);
    print_info(line);
    if (nds_backend_wait_session(resolved, data, size, msg, sizeof(msg)) == 0) {
        print_ok(msg);
        home_notice_set(msg);
    } else {
        print_warn(msg);
        home_notice_set(msg);
    }
}
#endif

/* ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
 * DISPATCH
 * ????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????? */
static void exec_command(void) {
    if(cmd_len==0) return;
    cmd_buf[cmd_len]='\0';
    tty_echo_cmd(cmd_buf);
    tokenize(cmd_buf);
    if(argc==0) return;

    if      (kstrcmp(argv[0],"help")   ==0) cmd_help();
    else if (kstrcmp(argv[0],"pwd")    ==0) cmd_pwd();
    else if (kstrcmp(argv[0],"cd")     ==0) cmd_cd();
    else if (kstrcmp(argv[0],"ls")     ==0) cmd_ls();
    else if (kstrcmp(argv[0],"mkdir")  ==0) cmd_mkdir();
    else if (kstrcmp(argv[0],"touch")  ==0) cmd_touch();
    else if (kstrcmp(argv[0],"open")   ==0) cmd_open();
    else if (kstrcmp(argv[0],"home")   ==0) cmd_home();
    else if (kstrcmp(argv[0],"disks")  ==0) cmd_disks();
    else if (kstrcmp(argv[0],"install")==0) cmd_install();
    else if (kstrcmp(argv[0],"persist")==0) cmd_persist();
    else if (kstrcmp(argv[0],"cat")    ==0) cmd_cat();
    else if (kstrcmp(argv[0],"write")  ==0) cmd_write();
    else if (kstrcmp(argv[0],"rm")     ==0) cmd_rm();
    else if (kstrcmp(argv[0],"run")    ==0) cmd_run();
    else if (kstrcmp(argv[0],"asm")    ==0) cmd_asm_editor();
    else if (kstrcmp(argv[0],"jackedit")==0) cmd_jack_editor();
    else if (kstrcmp(argv[0],"jackc")  ==0) cmd_jackc();
    else if (kstrcmp(argv[0],"jack")   ==0) cmd_jack();
    else if (kstrcmp(argv[0],"vmls")   ==0) cmd_vmls();
    else if (kstrcmp(argv[0],"dsdiag") ==0) cmd_dsdiag();
    else if (kstrcmp(argv[0],"ndsprobe")==0) cmd_ndsprobe();
    else if (kstrcmp(argv[0],"rominfo")==0) cmd_rominfo();
    else if (kstrcmp(argv[0],"audio")  ==0) cmd_audio();
    else if (kstrcmp(argv[0],"beep")   ==0) cmd_beep();
    else if (kstrcmp(argv[0],"ps")     ==0) cmd_ps();
    else if (kstrcmp(argv[0],"kill")   ==0) cmd_kill();
    else if (kstrcmp(argv[0],"free")   ==0) cmd_free();
    else if (kstrcmp(argv[0],"heap")   ==0) cmd_heap();
    else if (kstrcmp(argv[0],"uname")  ==0) cmd_uname();
    else if (kstrcmp(argv[0],"uptime") ==0) cmd_uptime();
    else if (kstrcmp(argv[0],"clear")  ==0) tty_clear_output();
    else if (kstrcmp(argv[0],"echo")   ==0) cmd_echo();
    else if (kstrcmp(argv[0],"newtask")==0) cmd_newtask();
    else {
        if (!launch_transparent_target(argv[0], 1)) {
            char msg[72]; ksprintf(msg,"bash: commande introuvable: %s",argv[0]);
            print_err(msg);
        }
    }
}

/* ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
 * BOUCLE PRINCIPALE
 * ????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????? */
void shell_run(void) {
    kstrncpy(g_cwd, "/home/root", RAMFS_MAX_PATH - 1);
    g_cwd[RAMFS_MAX_PATH - 1] = '\0';
    cursor_x = tty_prompt_len();
    cmd_len = 0;

    /* Démarrage direct sur le launcher graphique */
#ifdef ALOS_NDS_AUTOPROBE_ROM
    shell_run_nds_autoprobe();
    cmd_len = 0;
    cursor_x = tty_prompt_len();
    tty_clear_input();
#else
    if (bootinfo_installer_requested()) {
        installer_run_ui();
    } else if (tty_is_framebuffer()) {
        print_info("Mode UEFI/framebuffer detecte: shell texte actif.");
        tty_write("Tape 'install' pour l'installateur disque ou 'disks' pour voir les cibles.");
        tty_write("Le home graphique VGA est desactive dans ce mode pour garder un boot robuste.");
#if ALOS_HW_SAFE_GAMES
        tty_write("ROMs embarquees actives: tape 'vmls', puis 'open <jeu.gb/.gbc/.gba>'.");
#endif
    } else {
        launcher_home();
    }
    cmd_len = 0;
    cursor_x = tty_prompt_len();
    tty_clear_input();
#endif

    while(1){
        int handled = 0;
        while(keyboard_available()){
            handled = 1;
            uint8_t key=keyboard_getkey();
            if (key == KEY_F1) {
                cmd_len = 0;
                cursor_x = tty_prompt_len();
                tty_clear_input();
                if (tty_is_framebuffer()) {
                    print_warn("F1: home graphique VGA indisponible en mode UEFI/framebuffer.");
                } else {
                    launcher_home();
                }
                cmd_len = 0;
                cursor_x = tty_prompt_len();
                tty_clear_input();
            } else if(key==KEY_ENTER){
                exec_command(); cmd_len=0; cursor_x=tty_prompt_len(); tty_clear_input();
            } else if(key==KEY_BACKSPACE){
                if(cmd_len>0){cmd_len--;cursor_x--;
                    if(cursor_x<tty_prompt_len())cursor_x=tty_prompt_len();
                    tty_backspace(cursor_x);}
            } else if(key>=0x20&&key<0x80&&cmd_len<CMD_MAX){
                cmd_buf[cmd_len++]=(char)key; tty_echo_char((char)key,cursor_x++);
            }
        }
        if(!handled) {
#if ALOS_HW_SAFE
            usb_hid_kbd_poll();
            usb_xhci_poll();
            keyboard_poll();
            __asm__ volatile("pause");
#else
            __asm__ volatile("hlt");
#endif
        }
    }
}

