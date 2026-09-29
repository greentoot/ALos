/* driver/rtl8139.c - Pilote RTL8139 minimal (I/O-port, polling).
 * Voir rtl8139.h et kernel/net/net.c pour le contexte. */
#include "rtl8139.h"
#include "../kernel/memory/heap.h"
#include "../kernel/lib/string.h"

/* ── Acces bas niveau (port I/O + PCI config space), meme style que les
 * autres pilotes du noyau (chacun definit ses propres inb/outb statiques,
 * pas de header partage pour l'instant). ── */
static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile ("inb %1,%0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outw(uint16_t port, uint16_t v) { __asm__ volatile ("outw %0,%1" :: "a"(v), "Nd"(port)); }
static inline uint16_t inw(uint16_t port) { uint16_t v; __asm__ volatile ("inw %1,%0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile ("outl %0,%1" :: "a"(v), "Nd"(port)); }
static inline uint32_t inl(uint16_t port) { uint32_t v; __asm__ volatile ("inl %1,%0" : "=a"(v) : "Nd"(port)); return v; }

static uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                        ((uint32_t)func << 8) | (offset & 0xFCu);
    outl(0xCF8, address);
    return inl(0xCFC);
}

static void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                        ((uint32_t)func << 8) | (offset & 0xFCu);
    outl(0xCF8, address);
    outl(0xCFC, value);
}

/* ── Registres RTL8139 (offsets depuis la base I/O, BAR0). ── */
#define RTL_REG_MAC0    0x00
#define RTL_REG_RBSTART 0x30
#define RTL_REG_CMD     0x37
#define RTL_REG_CAPR    0x38
#define RTL_REG_IMR     0x3C
#define RTL_REG_ISR     0x3E
#define RTL_REG_TCR     0x40
#define RTL_REG_RCR     0x44
#define RTL_REG_CONFIG1 0x52
#define RTL_REG_TSAD0   0x20
#define RTL_REG_TSD0    0x10

#define RTL_CMD_RESET   0x10
#define RTL_CMD_RE      0x08
#define RTL_CMD_TE      0x04

#define RTL_RX_BUF_SIZE (8192u + 16u + 1500u) /* 8K anneau + marge WRAP */
#define RTL_TX_SLOTS    4u
#define RTL_TX_BUF_SIZE 1792u /* >= MTU Ethernet 1514 + marge, aligne */

static int      g_present = 0;
static uint16_t g_io = 0;
static uint8_t  g_mac[6];
static uint8_t *g_rx_buf = 0;
static uint32_t g_rx_offset = 0;
static uint8_t *g_tx_buf[RTL_TX_SLOTS];
static uint32_t g_tx_next = 0;

static int find_rtl8139(uint8_t *out_bus, uint8_t *out_slot, uint8_t *out_func) {
    for (uint16_t bus = 0; bus < 256u; ++bus) {
        for (uint8_t slot = 0; slot < 32u; ++slot) {
            for (uint8_t func = 0; func < 8u; ++func) {
                uint32_t vd = pci_config_read32((uint8_t)bus, slot, func, 0x00);
                uint16_t vendor = (uint16_t)(vd & 0xFFFFu);
                if (vendor == 0xFFFFu) continue;
                uint16_t device = (uint16_t)((vd >> 16) & 0xFFFFu);
                if (vendor == 0x10ECu && device == 0x8139u) {
                    *out_bus = (uint8_t)bus;
                    *out_slot = slot;
                    *out_func = func;
                    return 1;
                }
            }
        }
    }
    return 0;
}

int rtl8139_present(void) { return g_present; }
const uint8_t *rtl8139_mac(void) { return g_mac; }

int rtl8139_init(void) {
    uint8_t bus, slot, func;

    g_present = 0;
    if (!find_rtl8139(&bus, &slot, &func)) return 0;

    /* BAR0 : I/O port (bit0 = 1 pour un BAR I/O, adresse sur les bits hauts). */
    {
        uint32_t bar0 = pci_config_read32(bus, slot, func, 0x10);
        if ((bar0 & 0x1u) == 0) return 0; /* BAR0 pas en mode I/O : config inattendue, abandonner */
        g_io = (uint16_t)(bar0 & 0xFFFCu);
    }

    /* Bus mastering (necessaire pour que la carte fasse du DMA vers RBSTART/TSAD) + I/O. */
    {
        uint32_t cmd = pci_config_read32(bus, slot, func, 0x04);
        cmd = (cmd & 0xFFFF0000u) | (cmd & 0xFFFFu) | (1u << 2) | (1u << 0);
        pci_config_write32(bus, slot, func, 0x04, cmd);
    }

    outb(g_io + RTL_REG_CONFIG1, 0x00); /* sortie du mode veille */

    outb(g_io + RTL_REG_CMD, RTL_CMD_RESET);
    for (int i = 0; i < 1000000 && (inb(g_io + RTL_REG_CMD) & RTL_CMD_RESET); i++) {
        __asm__ volatile ("pause");
    }
    if (inb(g_io + RTL_REG_CMD) & RTL_CMD_RESET) return 0; /* reset jamais termine */

    for (int i = 0; i < 6; i++) g_mac[i] = inb((uint16_t)(g_io + RTL_REG_MAC0 + i));

    g_rx_buf = (uint8_t *)kmalloc(RTL_RX_BUF_SIZE);
    if (!g_rx_buf) return 0;
    kmemset(g_rx_buf, 0, RTL_RX_BUF_SIZE);
    for (uint32_t i = 0; i < RTL_TX_SLOTS; i++) {
        g_tx_buf[i] = (uint8_t *)kmalloc(RTL_TX_BUF_SIZE);
        if (!g_tx_buf[i]) return 0;
    }

    outl(g_io + RTL_REG_RBSTART, (uint32_t)(uintptr_t)g_rx_buf);

    outw(g_io + RTL_REG_IMR, 0x0005); /* ROK | TOK -- pas utilise en polling, mais laisse l'ISR se poser tranquillement */

    /* RCR : WRAP(bit7) | AB accepte broadcast(bit3) | AM accepte multicast(bit2) | APM accepte notre adresse physique(bit1). */
    outl(g_io + RTL_REG_RCR, 0x8Eu);

    outb(g_io + RTL_REG_CMD, RTL_CMD_RE | RTL_CMD_TE);

    /* TCR par defaut (pas de mode boucle, IFG standard) suffit pour QEMU. */
    outl(g_io + RTL_REG_TCR, 0x03000000u);

    g_rx_offset = 0;
    g_tx_next = 0;
    g_present = 1;
    return 1;
}

int rtl8139_send(const void *frame, uint32_t len) {
    if (!g_present || !frame || len == 0 || len > RTL_TX_BUF_SIZE) return 0;

    uint32_t slot = g_tx_next % RTL_TX_SLOTS;
    g_tx_next++;

    /* Attend que ce descripteur soit libre (OWN=1 signifie "libre a
     * reutiliser" cote pilote sur ce chipset -- on se contente ici
     * d'attendre TOK/OWN du precedent envoi sur ce slot avant de reecrire). */
    for (int i = 0; i < 200000; i++) {
        uint32_t tsd = inl((uint16_t)(g_io + RTL_REG_TSD0 + slot * 4u));
        if (tsd & (1u << 13)) break; /* OWN=1 : descripteur libre */
        __asm__ volatile ("pause");
    }

    kmemset(g_tx_buf[slot], 0, RTL_TX_BUF_SIZE);
    kmemcpy(g_tx_buf[slot], frame, len);
    uint32_t send_len = (len < 60u) ? 60u : len; /* trame Ethernet min 60 octets hors FCS */

    outl((uint16_t)(g_io + RTL_REG_TSAD0 + slot * 4u), (uint32_t)(uintptr_t)g_tx_buf[slot]);
    outl((uint16_t)(g_io + RTL_REG_TSD0 + slot * 4u), send_len & 0x1FFFu);

    return 1;
}

int rtl8139_poll_recv(void *buf, uint32_t bufsize) {
    uint16_t cmd;
    if (!g_present) return -1;

    cmd = inb(g_io + RTL_REG_CMD);
    if (cmd & 0x01u) return 0; /* BUFE=1 : anneau de reception vide */

    {
        uint8_t *hdr = g_rx_buf + g_rx_offset;
        uint16_t status = (uint16_t)(hdr[0] | (hdr[1] << 8));
        uint16_t pkt_len = (uint16_t)(hdr[2] | (hdr[3] << 8)); /* inclut le FCS (4 octets) */
        uint32_t copy_len;

        if (!(status & 0x0001u) /* ROK */ || pkt_len < 4u || pkt_len > 1600u) {
            /* Paquet corrompu ou compteur desynchronise : on ne peut pas se
             * fier au reste de l'anneau, mieux vaut reinitialiser CAPR sur
             * la position courante et abandonner ce paquet plutot que de
             * risquer une boucle infinie. */
            g_rx_offset = (g_rx_offset + 4u + pkt_len + 3u) & ~3u;
            if (g_rx_offset >= 8192u) g_rx_offset -= 8192u;
            outw(g_io + RTL_REG_CAPR, (uint16_t)(g_rx_offset - 16u));
            return 0;
        }

        copy_len = pkt_len - 4u; /* sans le FCS */
        if (copy_len > bufsize) copy_len = bufsize;
        kmemcpy(buf, hdr + 4, copy_len);

        g_rx_offset = (g_rx_offset + 4u + pkt_len + 3u) & ~3u;
        if (g_rx_offset >= 8192u) g_rx_offset -= 8192u;
        outw(g_io + RTL_REG_CAPR, (uint16_t)(g_rx_offset - 16u));

        return (int)copy_len;
    }
}
