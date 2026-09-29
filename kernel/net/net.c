/* kernel/net/net.c - Ethernet + ARP + ICMP echo minimal, au-dessus de
 * driver/rtl8139.c. Voir net.h pour la liste explicite de ce qui N'EST PAS
 * implemente (TCP/UDP, DHCP, IRQ...).
 *
 * Style delibere : tous les champs multi-octets des en-tetes reseau sont
 * des tableaux uint8_t (PAS des uint32_t/uint16_t), lus/ecrits via
 * rd16be/wr16be/rd32be/wr32be. x86 est petit-boutiste et le reseau est
 * gros-boutiste : un champ struct type uint16_t assigne directement
 * stockerait les octets dans le mauvais ordre. Cette approche evite la
 * classe de bug entiere plutot que de compter sur des htons/ntohs epars. */
#include "net.h"
#include "../../driver/rtl8139.h"
#include "../../driver/timer.h"
#include "../lib/string.h"
#include "../process/task.h"

/* IP fixe (pas de DHCP) : correspond a l'adresse que le reseau usermode
 * "SLIRP" de QEMU (-netdev user) attribuerait de toute facon au premier
 * client DHCP (10.0.2.15), donc utilisable telle quelle avec la commande
 * QEMU documentee dans le README SANS avoir a l'ajuster. Le "routeur"
 * virtuel SLIRP repond a ARP/ICMP sur 10.0.2.2 -- pratique pour tester
 * `net ping 10.0.2.2` sans dependre d'un service externe. */
static const uint8_t NET_LOCAL_IP[4] = { 10, 0, 2, 15 };

static uint8_t g_local_mac[6];
static int     g_net_available = 0;

/* Cache ARP a UNE seule entree (la derniere IP resolue) -- suffisant pour
 * `net ping` utilise de facon interactive, pas une vraie table ARP. */
static uint8_t g_arp_ip[4];
static uint8_t g_arp_mac[6];
static int     g_arp_valid = 0;

typedef struct __attribute__((packed)) {
    uint8_t dst[6];
    uint8_t src[6];
    uint8_t ethertype[2];
} EthHdr;

typedef struct __attribute__((packed)) {
    uint8_t htype[2];
    uint8_t ptype[2];
    uint8_t hlen;
    uint8_t plen;
    uint8_t oper[2];
    uint8_t sha[6];
    uint8_t spa[4];
    uint8_t tha[6];
    uint8_t tpa[4];
} ArpPkt;

typedef struct __attribute__((packed)) {
    uint8_t verihl;
    uint8_t tos;
    uint8_t totlen[2];
    uint8_t id[2];
    uint8_t flags_frag[2];
    uint8_t ttl;
    uint8_t proto;
    uint8_t checksum[2];
    uint8_t src[4];
    uint8_t dst[4];
} IpHdr;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t code;
    uint8_t checksum[2];
    uint8_t id[2];
    uint8_t seq[2];
} IcmpHdr;

#define ETHERTYPE_ARP  0x0806u
#define ETHERTYPE_IPV4 0x0800u
#define ARP_OP_REQUEST 1u
#define ARP_OP_REPLY   2u
#define IP_PROTO_ICMP  1u
#define ICMP_ECHO_REQUEST 8u
#define ICMP_ECHO_REPLY   0u
#define NET_PING_ICMP_ID  0xA105u /* identifiant fixe utilise par net_ping() pour reconnaitre ses propres reponses */

static void wr16be(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t rd16be(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

static uint16_t checksum16(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2; len -= 2;
    }
    if (len == 1) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFFu);
}

static int ip_eq(const uint8_t a[4], const uint8_t b[4]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static const uint8_t BCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

void net_init(void) {
    g_net_available = 0;
    g_arp_valid = 0;
    if (!rtl8139_init()) return;
    kmemcpy(g_local_mac, rtl8139_mac(), 6);
    g_net_available = 1;
}

int net_available(void) { return g_net_available; }
const uint8_t *net_local_ip(void) { return NET_LOCAL_IP; }
const uint8_t *net_local_mac(void) { return g_local_mac; }

static void send_arp_reply(const uint8_t dst_mac[6], const uint8_t dst_ip[4]) {
    uint8_t frame[sizeof(EthHdr) + sizeof(ArpPkt)];
    EthHdr *eth = (EthHdr *)frame;
    ArpPkt *arp = (ArpPkt *)(frame + sizeof(EthHdr));

    kmemcpy(eth->dst, dst_mac, 6);
    kmemcpy(eth->src, g_local_mac, 6);
    wr16be(eth->ethertype, ETHERTYPE_ARP);

    wr16be(arp->htype, 1);
    wr16be(arp->ptype, ETHERTYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    wr16be(arp->oper, ARP_OP_REPLY);
    kmemcpy(arp->sha, g_local_mac, 6);
    kmemcpy(arp->spa, NET_LOCAL_IP, 4);
    kmemcpy(arp->tha, dst_mac, 6);
    kmemcpy(arp->tpa, dst_ip, 4);

    rtl8139_send(frame, sizeof(frame));
}

static void send_arp_request(const uint8_t target_ip[4]) {
    uint8_t frame[sizeof(EthHdr) + sizeof(ArpPkt)];
    EthHdr *eth = (EthHdr *)frame;
    ArpPkt *arp = (ArpPkt *)(frame + sizeof(EthHdr));

    kmemcpy(eth->dst, BCAST_MAC, 6);
    kmemcpy(eth->src, g_local_mac, 6);
    wr16be(eth->ethertype, ETHERTYPE_ARP);

    wr16be(arp->htype, 1);
    wr16be(arp->ptype, ETHERTYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    wr16be(arp->oper, ARP_OP_REQUEST);
    kmemcpy(arp->sha, g_local_mac, 6);
    kmemcpy(arp->spa, NET_LOCAL_IP, 4);
    kmemset(arp->tha, 0, 6);
    kmemcpy(arp->tpa, target_ip, 4);

    rtl8139_send(frame, sizeof(frame));
}

/* echo_msg pointe sur le message ICMP RECU en entier (type/code/checksum/
 * id/seq/donnees, offsets standards -- voir la struct IcmpHdr), echo_msg_len
 * est sa taille totale (>= sizeof(IcmpHdr), verifie par l'appelant). */
static void send_icmp_echo_reply(const uint8_t dst_mac[6], const uint8_t dst_ip[4],
                                  const uint8_t *echo_msg, uint32_t echo_msg_len) {
    uint8_t frame[sizeof(EthHdr) + sizeof(IpHdr) + sizeof(IcmpHdr) + 1472];
    EthHdr *eth = (EthHdr *)frame;
    IpHdr *ip = (IpHdr *)(frame + sizeof(EthHdr));
    IcmpHdr *icmp = (IcmpHdr *)(frame + sizeof(EthHdr) + sizeof(IpHdr));
    uint8_t *data = frame + sizeof(EthHdr) + sizeof(IpHdr) + sizeof(IcmpHdr);
    uint32_t data_len;
    uint32_t icmp_len;

    data_len = (echo_msg_len > sizeof(IcmpHdr)) ? (echo_msg_len - sizeof(IcmpHdr)) : 0;
    if (data_len > sizeof(frame) - sizeof(EthHdr) - sizeof(IpHdr) - sizeof(IcmpHdr))
        data_len = sizeof(frame) - sizeof(EthHdr) - sizeof(IpHdr) - sizeof(IcmpHdr);

    kmemcpy(eth->dst, dst_mac, 6);
    kmemcpy(eth->src, g_local_mac, 6);
    wr16be(eth->ethertype, ETHERTYPE_IPV4);

    icmp->type = ICMP_ECHO_REPLY;
    icmp->code = 0;
    wr16be(icmp->checksum, 0);
    /* Offsets dans le message recu (struct IcmpHdr) : type=0 code=1
     * checksum=2..3 id=4..5 seq=6..7 donnees=8.. -- on renvoie le meme
     * id/seq/donnees que la requete, seul "type" change (echo -> reply). */
    kmemcpy(icmp->id,  echo_msg + 4, 2);
    kmemcpy(icmp->seq, echo_msg + 6, 2);
    if (data_len) kmemcpy(data, echo_msg + sizeof(IcmpHdr), data_len);
    icmp_len = sizeof(IcmpHdr) + data_len;
    wr16be(icmp->checksum, checksum16(icmp, icmp_len));

    ip->verihl = 0x45;
    ip->tos = 0;
    wr16be(ip->totlen, (uint16_t)(sizeof(IpHdr) + icmp_len));
    wr16be(ip->id, 0);
    wr16be(ip->flags_frag, 0);
    ip->ttl = 64;
    ip->proto = IP_PROTO_ICMP;
    wr16be(ip->checksum, 0);
    kmemcpy(ip->src, NET_LOCAL_IP, 4);
    kmemcpy(ip->dst, dst_ip, 4);
    wr16be(ip->checksum, checksum16(ip, sizeof(IpHdr)));

    rtl8139_send(frame, sizeof(EthHdr) + sizeof(IpHdr) + icmp_len);
}

static void send_icmp_echo_request(const uint8_t dst_mac[6], const uint8_t dst_ip[4],
                                    uint16_t id, uint16_t seq) {
    uint8_t frame[sizeof(EthHdr) + sizeof(IpHdr) + sizeof(IcmpHdr)];
    EthHdr *eth = (EthHdr *)frame;
    IpHdr *ip = (IpHdr *)(frame + sizeof(EthHdr));
    IcmpHdr *icmp = (IcmpHdr *)(frame + sizeof(EthHdr) + sizeof(IpHdr));

    kmemcpy(eth->dst, dst_mac, 6);
    kmemcpy(eth->src, g_local_mac, 6);
    wr16be(eth->ethertype, ETHERTYPE_IPV4);

    icmp->type = ICMP_ECHO_REQUEST;
    icmp->code = 0;
    wr16be(icmp->checksum, 0);
    wr16be(icmp->id, id);
    wr16be(icmp->seq, seq);
    wr16be(icmp->checksum, checksum16(icmp, sizeof(IcmpHdr)));

    ip->verihl = 0x45;
    ip->tos = 0;
    wr16be(ip->totlen, (uint16_t)(sizeof(IpHdr) + sizeof(IcmpHdr)));
    wr16be(ip->id, seq);
    wr16be(ip->flags_frag, 0);
    ip->ttl = 64;
    ip->proto = IP_PROTO_ICMP;
    wr16be(ip->checksum, 0);
    kmemcpy(ip->src, NET_LOCAL_IP, 4);
    kmemcpy(ip->dst, dst_ip, 4);
    wr16be(ip->checksum, checksum16(ip, sizeof(IpHdr)));

    rtl8139_send(frame, sizeof(frame));
}

/* Traite UNE trame recue (dispatch ARP/IPv4). Retourne 1 si une trame a
 * ete traitee, 0 si l'anneau de reception etait vide. "want_icmp_reply_id"
 * != -1 signale a l'appelant (net_ping) qu'un echo-reply avec cet id/seq
 * vient d'arriver, via *out_icmp_match. */
static int process_one_frame(int want_icmp_id, int want_icmp_seq, int *out_icmp_match) {
    static uint8_t buf[1600];
    int len = rtl8139_poll_recv(buf, sizeof(buf));
    if (out_icmp_match) *out_icmp_match = 0;
    if (len <= 0) return 0;
    if ((uint32_t)len < sizeof(EthHdr)) return 1;

    EthHdr *eth = (EthHdr *)buf;
    uint16_t ethertype = rd16be(eth->ethertype);

    if (ethertype == ETHERTYPE_ARP && (uint32_t)len >= sizeof(EthHdr) + sizeof(ArpPkt)) {
        ArpPkt *arp = (ArpPkt *)(buf + sizeof(EthHdr));
        uint16_t oper = rd16be(arp->oper);
        if (oper == ARP_OP_REQUEST && ip_eq(arp->tpa, NET_LOCAL_IP)) {
            send_arp_reply(arp->sha, arp->spa);
        } else if (oper == ARP_OP_REPLY) {
            kmemcpy(g_arp_ip, arp->spa, 4);
            kmemcpy(g_arp_mac, arp->sha, 6);
            g_arp_valid = 1;
        }
        return 1;
    }

    if (ethertype == ETHERTYPE_IPV4 && (uint32_t)len >= sizeof(EthHdr) + sizeof(IpHdr)) {
        IpHdr *ip = (IpHdr *)(buf + sizeof(EthHdr));
        uint32_t ihl = (uint32_t)(ip->verihl & 0x0Fu) * 4u;
        if (ip->proto == IP_PROTO_ICMP && (uint32_t)len >= sizeof(EthHdr) + ihl + sizeof(IcmpHdr)) {
            IcmpHdr *icmp = (IcmpHdr *)(buf + sizeof(EthHdr) + ihl);
            uint32_t icmp_total = (uint32_t)len - sizeof(EthHdr) - ihl;
            if (icmp->type == ICMP_ECHO_REQUEST && ip_eq(ip->dst, NET_LOCAL_IP)) {
                send_icmp_echo_reply(eth->src, ip->src, (const uint8_t *)icmp, icmp_total);
            } else if (icmp->type == ICMP_ECHO_REPLY) {
                if (want_icmp_id >= 0 &&
                    rd16be(icmp->id) == (uint16_t)want_icmp_id &&
                    rd16be(icmp->seq) == (uint16_t)want_icmp_seq &&
                    out_icmp_match) {
                    *out_icmp_match = 1;
                }
            }
        }
        return 1;
    }

    return 1;
}

void net_poll(void) {
    if (!g_net_available) return;
    /* Un seul paquet par appel : suffisant pour un usage "appele
     * regulierement depuis la boucle du shell", evite qu'un flot de
     * trafic entrant ne bloque le shell. */
    process_one_frame(-1, 0, 0);
}

int net_ping(const uint8_t ip[4], uint32_t timeout_ms) {
    static uint16_t seq_counter = 0;
    uint32_t deadline;
    uint32_t start;

    if (!g_net_available) return -1;

    if (!(g_arp_valid && ip_eq(g_arp_ip, ip))) {
        g_arp_valid = 0;
        send_arp_request(ip);
        deadline = timer_ms() + timeout_ms;
        while (timer_ms() < deadline) {
            process_one_frame(-1, 0, 0);
            if (g_arp_valid && ip_eq(g_arp_ip, ip)) break;
            task_sleep(1);
        }
        if (!(g_arp_valid && ip_eq(g_arp_ip, ip))) return -1; /* pas de reponse ARP */
    }

    seq_counter++;
    start = timer_ms();
    send_icmp_echo_request(g_arp_mac, ip, (uint16_t)NET_PING_ICMP_ID, seq_counter);

    deadline = start + timeout_ms;
    while (timer_ms() < deadline) {
        int matched = 0;
        process_one_frame((int)NET_PING_ICMP_ID, seq_counter, &matched);
        if (matched) return (int)(timer_ms() - start);
        task_sleep(1);
    }
    return -1;
}
