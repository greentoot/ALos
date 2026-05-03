/* kernel/boot/bootinfo.c - parse Multiboot info + basic platform probes */
#include "bootinfo.h"
#include "../lib/string.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289u
#define DEFAULT_MEM_END_BYTES      (16u * 1024u * 1024u)

/* multiboot_info (v1) layout: only the fields we consume. */
typedef struct __attribute__((packed)) {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    union {
        struct __attribute__((packed)) {
            uint32_t framebuffer_palette_addr;
            uint16_t framebuffer_palette_num_colors;
        } palette;
        struct __attribute__((packed)) {
            uint8_t framebuffer_red_field_position;
            uint8_t framebuffer_red_mask_size;
            uint8_t framebuffer_green_field_position;
            uint8_t framebuffer_green_mask_size;
            uint8_t framebuffer_blue_field_position;
            uint8_t framebuffer_blue_mask_size;
        } rgb;
    } color_info;
} mb_info_t;

typedef struct __attribute__((packed)) {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t string;
    uint32_t reserved;
} mb_module_t;

typedef struct __attribute__((packed)) {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} mb_mmap_t;

typedef struct __attribute__((packed)) {
    uint32_t total_size;
    uint32_t reserved;
} mb2_info_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
} mb2_tag_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char     string[];
} mb2_module_tag_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
} mb2_mmap_tag_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
} mb2_mmap_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;
    uint8_t  color_info[6];
} mb2_framebuffer_tag_t;

static BootInfo g_bootinfo;

static void copy_cstr_from_mb_addr(uint32_t addr, char *out, uint32_t out_sz) {
    const char *src = (const char *)(uintptr_t)addr;
    uint32_t i = 0;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!addr) return;
    while (src[i] && i + 1 < out_sz) {
        out[i] = src[i];
        i++;
    }
    out[i] = '\0';
}

static int cmdline_has_flag(const char *cmdline, const char *flag) {
    uint32_t lf;
    if (!cmdline || !flag || !*flag) return 0;
    lf = kstrlen(flag);
    while (*cmdline) {
        while (*cmdline == ' ') cmdline++;
        if (!*cmdline) break;
        if (kstrncmp(cmdline, flag, lf) == 0 && (cmdline[lf] == '\0' || cmdline[lf] == ' ')) {
            return 1;
        }
        while (*cmdline && *cmdline != ' ') cmdline++;
    }
    return 0;
}

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf));
}

static void probe_hypervisor(BootInfo *bi) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    bi->hypervisor_present = (uint8_t)((c >> 31) & 1u);

    bi->hypervisor_vendor[0] = '\0';
    if (!bi->hypervisor_present) return;

    cpuid(0x40000000u, &a, &b, &c, &d);
    kmemcpy(&bi->hypervisor_vendor[0], &b, 4);
    kmemcpy(&bi->hypervisor_vendor[4], &c, 4);
    kmemcpy(&bi->hypervisor_vendor[8], &d, 4);
    bi->hypervisor_vendor[12] = '\0';
}

static void bootinfo_finalize_common(BootInfo *bi) {
    bi->installer_requested = (uint8_t)cmdline_has_flag(bi->cmdline, "install=1");
}

static void bootinfo_set_default_fb_masks(BootInfo *bi) {
    if (!bi) return;
    if (bi->fb_bpp == 16) {
        bi->fb_red_pos = 11;
        bi->fb_red_mask = 5;
        bi->fb_green_pos = 5;
        bi->fb_green_mask = 6;
        bi->fb_blue_pos = 0;
        bi->fb_blue_mask = 5;
    } else {
        bi->fb_red_pos = 16;
        bi->fb_red_mask = 8;
        bi->fb_green_pos = 8;
        bi->fb_green_mask = 8;
        bi->fb_blue_pos = 0;
        bi->fb_blue_mask = 8;
    }
}

static void bootinfo_parse_mb1(const mb_info_t *mb) {
    g_bootinfo.multiboot_ok = 1;
    g_bootinfo.multiboot_flags = mb->flags;

    if (mb->flags & (1u << 0)) {
        g_bootinfo.mem_lower_kb = mb->mem_lower;
        g_bootinfo.mem_upper_kb = mb->mem_upper;
        if (mb->mem_upper) {
            g_bootinfo.mem_end_bytes = (mb->mem_upper + 1024u) * 1024u;
        }
    }

    if ((mb->flags & (1u << 2)) && mb->cmdline) {
        copy_cstr_from_mb_addr(mb->cmdline, g_bootinfo.cmdline, sizeof(g_bootinfo.cmdline));
    }

    if ((mb->flags & (1u << 3)) && mb->mods_addr && mb->mods_count) {
        const mb_module_t *mods = (const mb_module_t *)(uintptr_t)mb->mods_addr;
        uint32_t count = mb->mods_count;
        if (count > BOOTINFO_MAX_MODULES) count = BOOTINFO_MAX_MODULES;
        g_bootinfo.modules_count = count;
        for (uint32_t i = 0; i < count; i++) {
            g_bootinfo.modules[i].start = mods[i].mod_start;
            g_bootinfo.modules[i].end = mods[i].mod_end;
            copy_cstr_from_mb_addr(mods[i].string,
                                   g_bootinfo.modules[i].cmdline,
                                   sizeof(g_bootinfo.modules[i].cmdline));
        }
    }

    if ((mb->flags & (1u << 6)) && mb->mmap_addr && mb->mmap_length) {
        uint32_t pos = 0;
        uint32_t best_end = g_bootinfo.mem_end_bytes;
        const uint8_t *base = (const uint8_t *)(uintptr_t)mb->mmap_addr;

        while (pos + sizeof(mb_mmap_t) <= mb->mmap_length) {
            const mb_mmap_t *e = (const mb_mmap_t *)(const void *)(base + pos);
            uint32_t step = e->size + sizeof(e->size);
            if (!step || pos + step > mb->mmap_length) break;

            g_bootinfo.mmap_entries++;
            if (e->type == 1 && e->len) {
                uint64_t end64 = e->addr + e->len;
                if (end64 > (uint64_t)best_end) {
                    best_end = (end64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)end64;
                }
            }
            pos += step;
        }

        if (best_end > g_bootinfo.mem_end_bytes) g_bootinfo.mem_end_bytes = best_end;
    }

    if (mb->flags & (1u << 12)) {
        g_bootinfo.has_framebuffer = 1;
        g_bootinfo.fb_addr = mb->framebuffer_addr;
        g_bootinfo.fb_pitch = mb->framebuffer_pitch;
        g_bootinfo.fb_width = mb->framebuffer_width;
        g_bootinfo.fb_height = mb->framebuffer_height;
        g_bootinfo.fb_bpp = mb->framebuffer_bpp;
        g_bootinfo.fb_type = mb->framebuffer_type;
        if (mb->framebuffer_type == 1) {
            g_bootinfo.fb_red_pos = mb->color_info.rgb.framebuffer_red_field_position;
            g_bootinfo.fb_red_mask = mb->color_info.rgb.framebuffer_red_mask_size;
            g_bootinfo.fb_green_pos = mb->color_info.rgb.framebuffer_green_field_position;
            g_bootinfo.fb_green_mask = mb->color_info.rgb.framebuffer_green_mask_size;
            g_bootinfo.fb_blue_pos = mb->color_info.rgb.framebuffer_blue_field_position;
            g_bootinfo.fb_blue_mask = mb->color_info.rgb.framebuffer_blue_mask_size;
        }
        if (!g_bootinfo.fb_red_mask || !g_bootinfo.fb_green_mask || !g_bootinfo.fb_blue_mask) {
            bootinfo_set_default_fb_masks(&g_bootinfo);
        }
    }
}

static void bootinfo_parse_mb2(const mb2_info_t *mb2) {
    const uint8_t *base = (const uint8_t *)(const void *)mb2;
    uint32_t total = mb2->total_size;
    uint32_t off = 8;
    uint32_t best_end = g_bootinfo.mem_end_bytes;

    g_bootinfo.multiboot_ok = 1;
    g_bootinfo.multiboot_flags = 0x80000000u;

    while (off + sizeof(mb2_tag_t) <= total) {
        const mb2_tag_t *tag = (const mb2_tag_t *)(const void *)(base + off);
        uint32_t next = (off + tag->size + 7u) & ~7u;
        if (tag->size < sizeof(mb2_tag_t) || next > total) break;

        switch (tag->type) {
        case 0:
            off = total;
            continue;
        case 1: {
            const char *cmd = (const char *)(const void *)(tag + 1);
            uint32_t i = 0;
            while (cmd[i] && i + 1 < sizeof(g_bootinfo.cmdline)) {
                g_bootinfo.cmdline[i] = cmd[i];
                i++;
            }
            g_bootinfo.cmdline[i] = '\0';
            break;
        }
        case 3: {
            const mb2_module_tag_t *m = (const mb2_module_tag_t *)(const void *)tag;
            uint32_t idx = g_bootinfo.modules_count;
            uint32_t i = 0;
            if (idx < BOOTINFO_MAX_MODULES) {
                g_bootinfo.modules[idx].start = m->mod_start;
                g_bootinfo.modules[idx].end = m->mod_end;
                while (m->string[i] && i + 1 < sizeof(g_bootinfo.modules[idx].cmdline)) {
                    g_bootinfo.modules[idx].cmdline[i] = m->string[i];
                    i++;
                }
                g_bootinfo.modules[idx].cmdline[i] = '\0';
                g_bootinfo.modules_count++;
            }
            break;
        }
        case 4: {
            const uint32_t *mem = (const uint32_t *)(const void *)(tag + 1);
            g_bootinfo.mem_lower_kb = mem[0];
            g_bootinfo.mem_upper_kb = mem[1];
            if (g_bootinfo.mem_upper_kb) {
                g_bootinfo.mem_end_bytes = (g_bootinfo.mem_upper_kb + 1024u) * 1024u;
                best_end = g_bootinfo.mem_end_bytes;
            }
            break;
        }
        case 6: {
            const mb2_mmap_tag_t *mmap = (const mb2_mmap_tag_t *)(const void *)tag;
            uint32_t pos = sizeof(*mmap);
            while (pos + mmap->entry_size <= tag->size) {
                const mb2_mmap_entry_t *e = (const mb2_mmap_entry_t *)(const void *)((const uint8_t *)mmap + pos);
                g_bootinfo.mmap_entries++;
                if (e->type == 1 && e->len) {
                    uint64_t end64 = e->addr + e->len;
                    if (end64 > (uint64_t)best_end) {
                        best_end = (end64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)end64;
                    }
                }
                pos += mmap->entry_size;
            }
            if (best_end > g_bootinfo.mem_end_bytes) g_bootinfo.mem_end_bytes = best_end;
            break;
        }
        case 8: {
            const mb2_framebuffer_tag_t *fb = (const mb2_framebuffer_tag_t *)(const void *)tag;
            g_bootinfo.has_framebuffer = 1;
            g_bootinfo.fb_addr = fb->framebuffer_addr;
            g_bootinfo.fb_pitch = fb->framebuffer_pitch;
            g_bootinfo.fb_width = fb->framebuffer_width;
            g_bootinfo.fb_height = fb->framebuffer_height;
            g_bootinfo.fb_bpp = fb->framebuffer_bpp;
            g_bootinfo.fb_type = fb->framebuffer_type;
            if (fb->framebuffer_type == 1 && tag->size >= sizeof(*fb)) {
                g_bootinfo.fb_red_pos = fb->color_info[0];
                g_bootinfo.fb_red_mask = fb->color_info[1];
                g_bootinfo.fb_green_pos = fb->color_info[2];
                g_bootinfo.fb_green_mask = fb->color_info[3];
                g_bootinfo.fb_blue_pos = fb->color_info[4];
                g_bootinfo.fb_blue_mask = fb->color_info[5];
            }
            if (!g_bootinfo.fb_red_mask || !g_bootinfo.fb_green_mask || !g_bootinfo.fb_blue_mask) {
                bootinfo_set_default_fb_masks(&g_bootinfo);
            }
            break;
        }
        default:
            break;
        }
        off = next;
    }
}

void bootinfo_parse(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
    kmemset(&g_bootinfo, 0, sizeof(g_bootinfo));
    g_bootinfo.mem_end_bytes = DEFAULT_MEM_END_BYTES;

    probe_hypervisor(&g_bootinfo);

    if (!multiboot_info_addr) return;

    if (multiboot_magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        bootinfo_parse_mb1((const mb_info_t *)(uintptr_t)multiboot_info_addr);
    } else if (multiboot_magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        bootinfo_parse_mb2((const mb2_info_t *)(uintptr_t)multiboot_info_addr);
    } else {
        return;
    }

    bootinfo_finalize_common(&g_bootinfo);
}

const BootInfo *bootinfo_get(void) {
    return &g_bootinfo;
}

void bootinfo_override_framebuffer_addr(uint64_t fb_addr) {
    g_bootinfo.fb_addr = fb_addr;
}

const void *bootinfo_find_module(const char *cmdline, uint32_t *out_size) {
    if (out_size) *out_size = 0;
    if (!cmdline || !*cmdline) return 0;
    for (uint32_t i = 0; i < g_bootinfo.modules_count; i++) {
        const BootModuleInfo *m = &g_bootinfo.modules[i];
        if (kstrcmp(m->cmdline, cmdline) == 0) {
            if (out_size) *out_size = (m->end >= m->start) ? (m->end - m->start) : 0;
            return (const void *)(uintptr_t)m->start;
        }
    }
    return 0;
}

const char *bootinfo_cmdline(void) {
    return g_bootinfo.cmdline;
}

int bootinfo_installer_requested(void) {
    return g_bootinfo.installer_requested != 0;
}
