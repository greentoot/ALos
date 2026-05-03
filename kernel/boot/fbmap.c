#include "fbmap.h"
#include "bootinfo.h"

#define FBMAP_PAGE_2M         0x200000u
#define FBMAP_WINDOW_BASE     0xE0000000u
#define FBMAP_WINDOW_SIZE     (32u * 1024u * 1024u)
#define FBMAP_WINDOW_PDES     (FBMAP_WINDOW_SIZE / FBMAP_PAGE_2M)

#define CR0_PG                0x80000000u
#define CR4_PSE               0x00000010u
#define CR4_PAE               0x00000020u

#define PTE_PRESENT           0x001ull
#define PTE_WRITABLE          0x002ull
#define PDE_LARGE_PAGE        0x080ull

static uint64_t g_fbmap_pdpt[4] __attribute__((aligned(32)));
static uint64_t g_fbmap_identity_pd[4][512] __attribute__((aligned(4096)));
static int g_fbmap_initialized = 0;
static int g_fbmap_enabled = 0;

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf));
}

static int fbmap_cpu_supports_pae(void) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    (void)a; (void)b; (void)c;
    return (d & (1u << 6)) != 0;
}

static int fbmap_paging_enabled(void) {
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0,%0" : "=r"(cr0));
    return (cr0 & CR0_PG) != 0;
}

static void fbmap_build_identity_tables(void) {
    if (g_fbmap_initialized) return;

    for (uint32_t pdpt_i = 0; pdpt_i < 4; ++pdpt_i) {
        g_fbmap_pdpt[pdpt_i] =
            ((uint64_t)(uintptr_t)&g_fbmap_identity_pd[pdpt_i][0]) |
            PTE_PRESENT | PTE_WRITABLE;

        for (uint32_t pde_i = 0; pde_i < 512; ++pde_i) {
            uint64_t phys =
                (((uint64_t)pdpt_i * 512ull) + (uint64_t)pde_i) * (uint64_t)FBMAP_PAGE_2M;
            g_fbmap_identity_pd[pdpt_i][pde_i] =
                phys | PTE_PRESENT | PTE_WRITABLE | PDE_LARGE_PAGE;
        }
    }

    g_fbmap_initialized = 1;
}

static int fbmap_install_window(uint64_t phys_base, uint32_t bytes) {
    uint32_t virt_pde_base = (FBMAP_WINDOW_BASE >> 21) & 0x1FFu;
    uint32_t virt_pdpt = (FBMAP_WINDOW_BASE >> 30) & 0x3u;
    uint32_t pages = (bytes + FBMAP_PAGE_2M - 1u) / FBMAP_PAGE_2M;

    if (!pages || pages > FBMAP_WINDOW_PDES) return 0;
    if (virt_pde_base + pages > 512u) return 0;

    for (uint32_t i = 0; i < pages; ++i) {
        uint64_t phys = phys_base + (uint64_t)i * (uint64_t)FBMAP_PAGE_2M;
        g_fbmap_identity_pd[virt_pdpt][virt_pde_base + i] =
            phys | PTE_PRESENT | PTE_WRITABLE | PDE_LARGE_PAGE;
    }
    return 1;
}

static void fbmap_enable_paging(void) {
    uint32_t cr4;
    uint32_t cr0;
    uint32_t cr3 = (uint32_t)(uintptr_t)&g_fbmap_pdpt[0];

    if (g_fbmap_enabled) return;

    __asm__ volatile ("mov %%cr4,%0" : "=r"(cr4));
    cr4 |= (CR4_PSE | CR4_PAE);
    __asm__ volatile ("mov %0,%%cr4" :: "r"(cr4) : "memory");

    __asm__ volatile ("mov %0,%%cr3" :: "r"(cr3) : "memory");

    __asm__ volatile ("mov %%cr0,%0" : "=r"(cr0));
    cr0 |= CR0_PG;
    __asm__ volatile ("mov %0,%%cr0" :: "r"(cr0) : "memory");

    g_fbmap_enabled = 1;
}

uint64_t fbmap_prepare_framebuffer(const BootInfo *bi) {
    uint64_t phys_base;
    uint64_t offset64;
    uint32_t bytes;
    uint32_t map_bytes;

    if (!bi || !bi->has_framebuffer || !bi->fb_addr) return 0;
    if ((bi->fb_addr >> 32) == 0) return bi->fb_addr;
    if (!fbmap_cpu_supports_pae()) return 0;

    bytes = bi->fb_pitch * bi->fb_height;
    if (!bytes) return 0;

    phys_base = bi->fb_addr & ~((uint64_t)FBMAP_PAGE_2M - 1ull);
    offset64 = bi->fb_addr - phys_base;
    if (offset64 > 0xFFFFFFFFull) return 0;

    map_bytes = (uint32_t)offset64 + bytes;
    if (map_bytes > FBMAP_WINDOW_SIZE) return 0;

    fbmap_build_identity_tables();
    if (!fbmap_install_window(phys_base, map_bytes)) return 0;
    if (!fbmap_paging_enabled()) {
        fbmap_enable_paging();
    }

    return (uint64_t)FBMAP_WINDOW_BASE + offset64;
}
