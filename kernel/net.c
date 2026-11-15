#include "net.h"
#include "pci.h"
#include "io.h"
#include "input.h"
#include "serial.h"

/* PCnet-FAST III (Am79C973), the VirtualBox default NIC, also used by QEMU's
 * pcnet device. Polling only: the kernel does not take IRQs.
 * SWSTYLE 2 is the 32-bit software style. Descriptors are 16 bytes.
 */

#define PCNET_VENDOR 0x1022
#define PCNET_DEVICE 0x2000
#define DESC_OWN 0x80000000u
#define DESC_ERR 0x40000000u
#define DESC_STP 0x02000000u
#define DESC_ENP 0x01000000u
#define BUF_SIZE 1536
#define RING_N 4

struct desc {
    uint32_t addr;
    uint32_t flags;
    uint32_t misc;
    uint32_t reserved;
} __attribute__((packed));

static uint8_t *const arena = (uint8_t *)0x730000;
static struct desc *rxd;
static struct desc *txd;
static uint8_t *rxb;
static uint8_t *txb;
static uint8_t *initb;
static uint16_t nic_io;
static int nic_up;
static int tx_i;

static uint8_t mac[6];
static uint32_t ip_addr, ip_mask, ip_gw, ip_dns;
static uint16_t ip_id = 1;

struct arp_ent { uint32_t ip; uint8_t mac[6]; int used; };
static struct arp_ent arp_tab[8];

static uint32_t ping_wait_ip;
static uint16_t ping_wait_seq;
static int ping_got;
static uint32_t ping_tick;

static uint32_t dns_wait_id;
static uint32_t dns_result;
static int dns_got;

enum { TCP_CLOSED = 0, TCP_SYN, TCP_EST, TCP_FIN };
static struct {
    int state;
    uint32_t rip;
    uint16_t lport, rport;
    uint32_t snd_una, snd_nxt, rcv_nxt;
    uint8_t rmac[6];
    uint8_t last[1600];
    int last_len;
    uint32_t last_tick;
    int retries;
    uint8_t *recv;
    int recv_max, recv_len;
    int fin_seen;
} tcp;

static uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static uint32_t htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}
static uint16_t ntohs(uint16_t v) { return htons(v); }
static uint32_t ntohl(uint32_t v) { return htonl(v); }

static uint16_t checksum(const void *data, int len, uint32_t sum) {
    const uint8_t *p = data;
    while (len > 1) {
        sum += (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint16_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void nic_w_csr(uint16_t reg, uint16_t val) {
    outw((uint16_t)(nic_io + 0x12), reg);
    outw((uint16_t)(nic_io + 0x10), val);
}
static uint16_t nic_r_csr(uint16_t reg) {
    outw((uint16_t)(nic_io + 0x12), reg);
    return inw((uint16_t)(nic_io + 0x10));
}
static void nic_w_bcr(uint16_t reg, uint16_t val) {
    outw((uint16_t)(nic_io + 0x12), reg);
    outw((uint16_t)(nic_io + 0x16), val);
}

static void rx_give(int i) {
    rxd[i].addr = (uint32_t)(uintptr_t)(rxb + (uint32_t)i * BUF_SIZE);
    rxd[i].flags = DESC_OWN | 0xF000u | ((uint32_t)(-BUF_SIZE) & 0xFFFu);
    rxd[i].misc = 0;
    rxd[i].reserved = 0;
}

static int nic_tx(const void *frame, int len) {
    if (!nic_up || len < 1) return -1;
    if (len < 60) len = 60;
    if (len > BUF_SIZE) return -1;
    for (int spin = 0; spin < 10000; spin++) {
        if (!(txd[tx_i].flags & DESC_OWN)) break;
        input_timer();
    }
    if (txd[tx_i].flags & DESC_OWN) return -1;
    uint8_t *buf = txb + (uint32_t)tx_i * BUF_SIZE;
    memset(buf, 0, 60);
    memcpy(buf, frame, (size_t)len);
    txd[tx_i].addr = (uint32_t)(uintptr_t)buf;
    txd[tx_i].misc = 0;
    txd[tx_i].reserved = 0;
    txd[tx_i].flags = DESC_OWN | DESC_STP | DESC_ENP | 0xF000u | ((uint32_t)(-len) & 0xFFFu);
    nic_w_csr(0, 0x0008);
    tx_i = (tx_i + 1) & (RING_N - 1);
    return 0;
}

static int arp_lookup(uint32_t ip, uint8_t out[6]) {
    for (int i = 0; i < 8; i++) {
        if (arp_tab[i].used && arp_tab[i].ip == ip) {
            memcpy(out, arp_tab[i].mac, 6);
            return 0;
        }
    }
    return -1;
}

static void arp_store(uint32_t ip, const uint8_t *m) {
    if (!ip || ip == 0xFFFFFFFFu) return;
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (arp_tab[i].used && arp_tab[i].ip == ip) { slot = i; break; }
        if (!arp_tab[i].used) slot = i;
    }
    arp_tab[slot].used = 1;
    arp_tab[slot].ip = ip;
    memcpy(arp_tab[slot].mac, m, 6);
}

static void eth_send(const uint8_t *dst, uint16_t type, const void *payload, int len) {
    uint8_t frame[1514];
    if (len < 0 || len > 1500) return;
    memcpy(frame, dst, 6);
    memcpy(frame + 6, mac, 6);
    put16(frame + 12, type);
    memcpy(frame + 14, payload, (size_t)len);
    nic_tx(frame, 14 + len);
}

static void arp_send(int reply, const uint8_t *dst_mac, uint32_t dst_ip) {
    uint8_t p[28];
    memset(p, 0, sizeof(p));
    put16(p, 1);
    put16(p + 2, 0x0800);
    p[4] = 6;
    p[5] = 4;
    put16(p + 6, reply ? 2 : 1);
    memcpy(p + 8, mac, 6);
    put32(p + 14, ip_addr);
    if (dst_mac) memcpy(p + 18, dst_mac, 6);
    put32(p + 24, dst_ip);
    uint8_t bcast[6];
    memset(bcast, 0xFF, 6);
    eth_send(reply && dst_mac ? dst_mac : bcast, 0x0806, p, 28);
}

static void ip_send_to(const uint8_t *dst_mac, uint32_t dst, uint8_t proto, const void *payload, int len);

static int arp_resolve(uint32_t ip, uint8_t out[6]) {
    if (!arp_lookup(ip, out)) return 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        arp_send(0, 0, ip);
        uint32_t start = timer_ticks();
        while (timer_ticks() - start < 30) {
            net_service();
            if (!arp_lookup(ip, out)) return 0;
        }
    }
    return -1;
}

static void ip_send_to(const uint8_t *dst_mac, uint32_t dst, uint8_t proto, const void *payload, int len) {
    uint8_t pkt[1500];
    if (len < 0 || 20 + len > (int)sizeof(pkt)) return;
    memset(pkt, 0, 20);
    pkt[0] = 0x45;
    put16(pkt + 2, (uint16_t)(20 + len));
    put16(pkt + 4, ip_id++);
    pkt[8] = 64;
    pkt[9] = proto;
    put32(pkt + 12, ip_addr);
    put32(pkt + 16, dst);
    put16(pkt + 10, checksum(pkt, 20, 0));
    memcpy(pkt + 20, payload, (size_t)len);
    eth_send(dst_mac, 0x0800, pkt, 20 + len);
}

static int ip_send(uint32_t dst, uint8_t proto, const void *payload, int len) {
    uint8_t dmac[6];
    if (dst == 0xFFFFFFFFu) {
        memset(dmac, 0xFF, 6);
        ip_send_to(dmac, dst, proto, payload, len);
        return 0;
    }
    uint32_t next = dst;
    if (ip_mask && ((dst ^ ip_addr) & ip_mask)) next = ip_gw;
    if (!next) return -1;
    if (arp_resolve(next, dmac)) {
        if (!ip_gw || next == ip_gw || arp_resolve(ip_gw, dmac)) return -1;
    }
    ip_send_to(dmac, dst, proto, payload, len);
    return 0;
}

static uint16_t transport_sum(uint32_t src, uint32_t dst, uint8_t proto, const void *payload, int len) {
    uint8_t pseudo[12];
    put32(pseudo, src);
    put32(pseudo + 4, dst);
    pseudo[8] = 0;
    pseudo[9] = proto;
    put16(pseudo + 10, (uint16_t)len);
    uint32_t sum = (uint16_t)~checksum(pseudo, 12, 0);
    return checksum(payload, len, sum);
}

static int udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, int len) {
    uint8_t buf[1472];
    if (len < 0 || 8 + len > (int)sizeof(buf)) return -1;
    put16(buf, sport);
    put16(buf + 2, dport);
    put16(buf + 4, (uint16_t)(8 + len));
    put16(buf + 6, 0);
    memcpy(buf + 8, data, (size_t)len);
    put16(buf + 6, transport_sum(ip_addr, dst, 17, buf, 8 + len));
    return ip_send(dst, 17, buf, 8 + len);
}

static void tcp_send_raw(uint8_t flags, const void *data, int len, int save) {
    uint8_t buf[1500];
    int hdr = (flags & 0x02) ? 24 : 20;
    if (len < 0 || hdr + len > (int)sizeof(buf)) return;
    memset(buf, 0, (size_t)hdr);
    put16(buf, tcp.lport);
    put16(buf + 2, tcp.rport);
    put32(buf + 4, tcp.snd_nxt);
    put32(buf + 8, tcp.rcv_nxt);
    buf[12] = (uint8_t)((hdr / 4) << 4);
    buf[13] = flags;
    put16(buf + 14, 8192);
    if (flags & 0x02) {
        buf[20] = 2;
        buf[21] = 4;
        put16(buf + 22, 1460);
    }
    if (len && data) memcpy(buf + hdr, data, (size_t)len);
    put16(buf + 16, transport_sum(ip_addr, tcp.rip, 6, buf, hdr + len));
    if (save) {
        memcpy(tcp.last, buf, (size_t)(hdr + len));
        tcp.last_len = hdr + len;
        tcp.last_tick = timer_ticks();
        tcp.retries = 0;
    }
    ip_send_to(tcp.rmac, tcp.rip, 6, buf, hdr + len);
}

static void tcp_input(const uint8_t *p, int len, uint32_t src) {
    if (len < 20 || tcp.state == TCP_CLOSED) return;
    if (src != tcp.rip) return;
    if (get16(p) != tcp.rport || get16(p + 2) != tcp.lport) return;
    uint32_t seq = get32(p + 4);
    uint32_t ack = get32(p + 8);
    int hlen = ((p[12] >> 4) & 0xF) * 4;
    if (hlen < 20 || hlen > len) return;
    uint8_t flags = p[13];
    if (flags & 0x04) { tcp.state = TCP_CLOSED; return; }
    if (flags & 0x10) {
        if (ack > tcp.snd_una && ack <= tcp.snd_nxt + 1) tcp.snd_una = ack;
    }
    if (tcp.state == TCP_SYN && (flags & 0x12) == 0x12 && ack == tcp.snd_nxt) {
        tcp.rcv_nxt = seq + 1;
        tcp.snd_una = ack;
        tcp.state = TCP_EST;
        tcp_send_raw(0x10, 0, 0, 0);
        return;
    }
    int plen = len - hlen;
    if (tcp.state == TCP_EST || tcp.state == TCP_FIN) {
        if (plen > 0 && seq == tcp.rcv_nxt) {
            int room = tcp.recv_max - tcp.recv_len;
            int take = plen < room ? plen : room;
            if (take > 0 && tcp.recv) {
                memcpy(tcp.recv + tcp.recv_len, p + hlen, (size_t)take);
                tcp.recv_len += take;
            }
            tcp.rcv_nxt += (uint32_t)plen;
            tcp_send_raw(0x10, 0, 0, 0);
        } else if (plen > 0) {
            tcp_send_raw(0x10, 0, 0, 0);
        }
        if (flags & 0x01) {
            if (seq + (uint32_t)plen == tcp.rcv_nxt) {
                tcp.rcv_nxt += 1;
                tcp.fin_seen = 1;
                tcp_send_raw(0x10, 0, 0, 0);
            }
        }
    }
}

static void icmp_input(const uint8_t *p, int len, uint32_t src) {
    if (len < 8) return;
    if (p[0] == 0 && ping_wait_ip == src && get16(p + 6) == ping_wait_seq) {
        ping_got = 1;
        return;
    }
    if (p[0] != 8) return;
    uint8_t reply[1500];
    if (len > (int)sizeof(reply)) return;
    memcpy(reply, p, (size_t)len);
    reply[0] = 0;
    put16(reply + 2, 0);
    put16(reply + 2, checksum(reply, len, 0));
    ip_send(src, 1, reply, len);
}

static int skip_dns_name(const uint8_t *data, int len, int i);
static void udp_input(const uint8_t *p, int len, uint32_t src);
static void dhcp_input(const uint8_t *p, int len, uint32_t src);

static void ip_input(const uint8_t *p, int len) {
    if (len < 20) return;
    if ((p[0] >> 4) != 4) return;
    int hlen = (p[0] & 0xF) * 4;
    int total = get16(p + 2);
    if (hlen < 20 || total > len || total < hlen) return;
    uint8_t tmp[20];
    if (hlen <= 60) {
        memcpy(tmp, p, (size_t)hlen);
        if (checksum(tmp, hlen, 0) != 0 && checksum(p, hlen, 0) != 0xFFFF && checksum(p, hlen, 0) != 0)
            return;
    }
    uint32_t src = get32(p + 12);
    uint32_t dst = get32(p + 16);
    if (dst != ip_addr && dst != 0xFFFFFFFFu && ip_addr != 0) return;
    const uint8_t *pl = p + hlen;
    int plen = total - hlen;
    if (p[9] == 1) icmp_input(pl, plen, src);
    else if (p[9] == 17) udp_input(pl, plen, src);
    else if (p[9] == 6) tcp_input(pl, plen, src);
}

static void arp_input(const uint8_t *p, int len, const uint8_t *smac) {
    (void)smac;
    if (len < 28) return;
    if (get16(p) != 1 || get16(p + 2) != 0x0800) return;
    uint16_t op = get16(p + 6);
    uint32_t sip = get32(p + 14);
    uint32_t tip = get32(p + 24);
    arp_store(sip, p + 8);
    if (op == 1 && tip == ip_addr && ip_addr) arp_send(1, p + 8, sip);
}

static void net_rx(const uint8_t *frame, int len) {
    if (len < 14) return;
    uint16_t type = get16(frame + 12);
    if (type == 0x0806) arp_input(frame + 14, len - 14, frame + 6);
    else if (type == 0x0800) ip_input(frame + 14, len - 14);
}

static void nic_poll(void) {
    if (!nic_up) return;
    for (int n = 0; n < RING_N; n++) {
        for (int i = 0; i < RING_N; i++) {
            uint32_t f = rxd[i].flags;
            if (f & DESC_OWN) continue;
            int mcnt = (int)(rxd[i].misc & 0xFFFu);
            if (!(f & DESC_ERR) && (f & DESC_STP) && (f & DESC_ENP) && mcnt >= 14 && mcnt <= BUF_SIZE)
                net_rx(rxb + (uint32_t)i * BUF_SIZE, mcnt);
            rx_give(i);
        }
    }
}

void net_service(void) {
    input_timer();
    nic_poll();
    if (tcp.state != TCP_CLOSED && tcp.last_len && tcp.snd_una < tcp.snd_nxt) {
        if (timer_ticks() - tcp.last_tick > 40 && tcp.retries < 6) {
            ip_send_to(tcp.rmac, tcp.rip, 6, tcp.last, tcp.last_len);
            tcp.last_tick = timer_ticks();
            tcp.retries++;
        }
    }
}

static int nic_init(void) {
    int bus, dev, fn;
    if (pci_find(PCNET_VENDOR, PCNET_DEVICE, &bus, &dev, &fn)) {
        if (pci_find(PCNET_VENDOR, 0x2001, &bus, &dev, &fn)) return -1;
    }
    uint32_t bar = pci_read(bus, dev, fn, 0x10);
    if (!(bar & 1u)) return -1;
    nic_io = (uint16_t)(bar & ~3u);
    uint32_t cmd = pci_read(bus, dev, fn, 4);
    pci_write(bus, dev, fn, 4, cmd | 0x5u);

    inw((uint16_t)(nic_io + 0x14));
    for (int i = 0; i < 10000; i++) io_wait();
    for (int i = 0; i < 6; i++) mac[i] = inb((uint16_t)(nic_io + i));
    if (mac[0] == 0 && mac[1] == 0 && mac[2] == 0) return -1;

    initb = arena;
    rxd = (struct desc *)(arena + 0x100);
    txd = (struct desc *)(arena + 0x200);
    rxb = arena + 0x1000;
    txb = arena + 0x8000;
    memset(initb, 0, 32);
    memset(rxd, 0, sizeof(struct desc) * RING_N);
    memset(txd, 0, sizeof(struct desc) * RING_N);
    for (int i = 0; i < RING_N; i++) rx_give(i);

    initb[2] = (uint8_t)(2 << 4);
    initb[3] = (uint8_t)(2 << 4);
    memcpy(initb + 4, mac, 6);
    uint32_t rdra = (uint32_t)(uintptr_t)rxd;
    uint32_t tdra = (uint32_t)(uintptr_t)txd;
    initb[20] = (uint8_t)rdra;
    initb[21] = (uint8_t)(rdra >> 8);
    initb[22] = (uint8_t)(rdra >> 16);
    initb[23] = (uint8_t)(rdra >> 24);
    initb[24] = (uint8_t)tdra;
    initb[25] = (uint8_t)(tdra >> 8);
    initb[26] = (uint8_t)(tdra >> 16);
    initb[27] = (uint8_t)(tdra >> 24);

    nic_w_csr(0, 0x0004);
    nic_w_bcr(20, 2);
    uint32_t ib = (uint32_t)(uintptr_t)initb;
    nic_w_csr(1, (uint16_t)ib);
    nic_w_csr(2, (uint16_t)(ib >> 16));
    nic_w_csr(0, 0x0001);
    int idon = 0;
    for (int i = 0; i < 100000; i++) {
        if (nic_r_csr(0) & 0x0100) { idon = 1; break; }
    }
    if (!idon) return -1;
    nic_w_csr(0, 0x0002);
    nic_up = 1;
    tx_i = 0;
    serial_write("NET: pcnet ");
    for (int i = 0; i < 6; i++) {
        static const char hex[] = "0123456789ABCDEF";
        char b[3] = { hex[mac[i] >> 4], hex[mac[i] & 0xF], 0 };
        serial_write(b);
        if (i != 5) serial_write(":");
    }
    serial_write("\n");
    return 0;
}

static uint32_t dhcp_xid;
static int dhcp_state;
static uint32_t dhcp_offer, dhcp_server;

static void dhcp_send(int kind) {
    uint8_t p[300];
    memset(p, 0, sizeof(p));
    p[0] = 1;
    p[1] = 1;
    p[2] = 6;
    put32(p + 4, dhcp_xid);
    put16(p + 10, 0x8000);
    memcpy(p + 28, mac, 6);
    p[236] = 99; p[237] = 130; p[238] = 83; p[239] = 99;
    int o = 240;
    p[o++] = 53; p[o++] = 1; p[o++] = (uint8_t)kind;
    if (kind == 3 && dhcp_offer) {
        p[o++] = 50; p[o++] = 4;
        put32(p + o, dhcp_offer); o += 4;
        p[o++] = 54; p[o++] = 4;
        put32(p + o, dhcp_server); o += 4;
    }
    p[o++] = 55; p[o++] = 3; p[o++] = 1; p[o++] = 3; p[o++] = 6;
    p[o++] = 255;
    udp_send(0xFFFFFFFFu, 68, 67, p, o);
}

static void dhcp_input(const uint8_t *p, int len, uint32_t src) {
    (void)src;
    if (len < 244) return;
    if (p[236] != 99 || p[237] != 130 || p[238] != 83 || p[239] != 99) return;
    if (get32(p + 4) != dhcp_xid) return;
    int kind = 0;
    uint32_t server = 0, mask = 0, gw = 0, dns = 0;
    for (int i = 240; i + 1 < len && p[i] != 255; ) {
        uint8_t t = p[i++];
        if (t == 0) continue;
        if (i >= len) break;
        int l = p[i++];
        if (i + l > len) break;
        if (t == 53 && l >= 1) kind = p[i];
        else if (t == 1 && l >= 4) mask = get32(p + i);
        else if (t == 3 && l >= 4) gw = get32(p + i);
        else if (t == 6 && l >= 4) dns = get32(p + i);
        else if (t == 54 && l >= 4) server = get32(p + i);
        i += l;
    }
    uint32_t yi = get32(p + 16);
    if (kind == 2 && yi) {
        dhcp_offer = yi;
        dhcp_server = server;
        ip_mask = mask;
        ip_gw = gw;
        ip_dns = dns;
        dhcp_state = 2;
    } else if (kind == 5 && yi) {
        ip_addr = yi;
        if (mask) ip_mask = mask;
        if (gw) ip_gw = gw;
        if (dns) ip_dns = dns;
        if (!ip_mask) ip_mask = 0xFFFFFF00u;
        dhcp_state = 5;
    }
}

static void udp_input(const uint8_t *p, int len, uint32_t src) {
    if (len < 8) return;
    uint16_t sport = get16(p);
    uint16_t dport = get16(p + 2);
    int ulen = get16(p + 4);
    if (ulen < 8 || ulen > len) ulen = len;
    const uint8_t *data = p + 8;
    int dlen = ulen - 8;
    if (dport == 68 && sport == 67) dhcp_input(data, dlen, src);
    if (dport == dns_wait_id && sport == 53 && dns_wait_id) {
        /* DNS id is in the payload, not the port. Handled below by port 53. */
    }
    if (sport == 53 && dlen >= 12 && (dport == 1053 || dport == (uint16_t)dns_wait_id)) {
        uint16_t id = get16(data);
        if (id == (uint16_t)dns_wait_id) {
            int qd = get16(data + 4);
            int an = get16(data + 6);
            int i = 12;
            while (qd-- > 0 && i < dlen) {
                i = skip_dns_name(data, dlen, i);
                if (i < 0) return;
                i += 4;
            }
            while (an-- > 0 && i < dlen) {
                i = skip_dns_name(data, dlen, i);
                if (i < 0 || i + 10 > dlen) return;
                uint16_t typ = get16(data + i);
                uint16_t rdlen = get16(data + i + 8);
                i += 10;
                if (i + rdlen > dlen) return;
                if (typ == 1 && rdlen == 4) {
                    dns_result = get32(data + i);
                    dns_got = 1;
                    return;
                }
                /* CNAME and other types: keep scanning for an A record. */
                i += rdlen;
            }
        }
    }
}

static int dhcp_run(void) {
    dhcp_xid = 0x474F5300u ^ timer_ticks() ^ ((uint32_t)mac[4] << 8) ^ mac[5];
    dhcp_state = 1;
    dhcp_offer = 0;
    ip_addr = 0;
    for (int attempt = 0; attempt < 4 && dhcp_state < 5; attempt++) {
        dhcp_send(1);
        uint32_t start = timer_ticks();
        while (dhcp_state < 2 && timer_ticks() - start < 80) net_service();
        if (dhcp_state < 2) continue;
        dhcp_send(3);
        start = timer_ticks();
        while (dhcp_state < 5 && timer_ticks() - start < 80) net_service();
    }
    if (dhcp_state != 5) return -1;
    serial_write("NET: ip ");
    char b[20];
    net_format_ip(ip_addr, b, 20);
    serial_write(b);
    serial_write(" dns ");
    if (!ip_dns) ip_dns = 0x0A000203u;
    if (!ip_gw) ip_gw = 0x0A000202u;
    net_format_ip(ip_dns, b, 20);
    serial_write(b);
    serial_write("\n");
    arp_store(ip_gw, 0);
    /* Gateway MAC is filled by the first ARP. Drop the empty store. */
    for (int i = 0; i < 8; i++) if (arp_tab[i].ip == ip_gw) arp_tab[i].used = 0;
    return 0;
}

int net_init(void) {
    if (nic_up && ip_addr) return 0;
    if (!nic_up) {
        if (nic_init()) {
            serial_write("NET: no pcnet\n");
            return -1;
        }
    }
    if (dhcp_run()) {
        serial_write("NET: dhcp failed\n");
        return -1;
    }
    return 0;
}

int net_ready(void) { return nic_up && ip_addr; }
uint32_t net_ip(void) { return ip_addr; }
uint32_t net_mask(void) { return ip_mask; }
uint32_t net_gateway(void) { return ip_gw; }
uint32_t net_dns(void) { return ip_dns; }
const uint8_t *net_mac(void) { return mac; }

void net_format_ip(uint32_t ip, char *dst, int n) {
    if (!dst || n < 8) return;
    int o = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t v = (ip >> (24 - 8 * i)) & 0xFF;
        char tmp[4];
        int t = 0;
        if (!v) tmp[t++] = '0';
        else {
            char rev[4]; int r = 0;
            while (v) { rev[r++] = (char)('0' + v % 10); v /= 10; }
            while (r) tmp[t++] = rev[--r];
        }
        for (int k = 0; k < t && o + 1 < n; k++) dst[o++] = tmp[k];
        if (i != 3 && o + 1 < n) dst[o++] = '.';
    }
    dst[o] = 0;
}

static int parse_ip(const char *s, uint32_t *out) {
    uint32_t ip = 0;
    for (int i = 0; i < 4; i++) {
        if (s[0] < '0' || s[0] > '9') return -1;
        int v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            if (v > 255) return -1;
            s++;
        }
        ip = (ip << 8) | (uint32_t)v;
        if (i < 3) {
            if (*s != '.') return -1;
            s++;
        }
    }
    if (*s) return -1;
    *out = ip;
    return 0;
}

static int skip_dns_name(const uint8_t *data, int len, int i) {
    int guard = 0;
    while (i < len && guard++ < 64) {
        uint8_t n = data[i];
        if (n == 0) return i + 1;
        if ((n & 0xC0) == 0xC0) return (i + 2 <= len) ? i + 2 : -1;
        if ((n & 0xC0) != 0) return -1;
        if (i + 1 + n >= len) return -1;
        i += 1 + n;
    }
    return -1;
}

static int dns_query_one(const char *host, uint32_t server, uint32_t *ip_out) {
    uint8_t q[256];
    memset(q, 0, sizeof(q));
    uint16_t id = (uint16_t)(0x4000u ^ (timer_ticks() & 0xFFFF) ^ (server & 0xFFFF));
    if (id < 1024) id = (uint16_t)(id + 2000);
    put16(q, id);
    q[2] = 0x01; /* recursion desired */
    put16(q + 4, 1); /* one question */
    int o = 12;
    const char *p = host;
    while (*p && o < 220) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int n = (int)(dot - p);
        if (n <= 0 || n > 63) return -1;
        q[o++] = (uint8_t)n;
        memcpy(q + o, p, (size_t)n);
        o += n;
        p = *dot == '.' ? dot + 1 : dot;
    }
    q[o++] = 0;
    put16(q + o, 1); o += 2; /* A */
    put16(q + o, 1); o += 2; /* IN */
    char sip[20];
    net_format_ip(server, sip, 20);
    serial_write("DNS: query ");
    serial_write(host);
    serial_write(" via ");
    serial_write(sip);
    serial_write("\n");
    dns_wait_id = id;
    dns_got = 0;
    dns_result = 0;
    for (int a = 0; a < 4 && !dns_got; a++) {
        if (udp_send(server, 1053, 53, q, o)) serial_write("DNS: send failed\n");
        uint32_t start = timer_ticks();
        while (!dns_got && timer_ticks() - start < 120) net_service();
    }
    dns_wait_id = 0;
    if (!dns_got) {
        serial_write("DNS: no answer\n");
        return -1;
    }
    *ip_out = dns_result;
    serial_write("DNS: answer ");
    net_format_ip(dns_result, sip, 20);
    serial_write(sip);
    serial_write("\n");
    return 0;
}

int net_resolve(const char *host, uint32_t *ip_out) {
    if (!host || !ip_out) return -1;
    if (!parse_ip(host, ip_out)) return 0;
    if (!net_ready() && net_init()) return -1;
    if (!net_ready()) return -1;
    uint32_t servers[4];
    int ns = 0;
    if (ip_dns) servers[ns++] = ip_dns;
    servers[ns++] = 0x0A000203u; /* 10.0.2.3, VirtualBox and QEMU NAT */
    servers[ns++] = 0x08080808u;
    servers[ns++] = 0x01010101u;
    for (int i = 0; i < ns; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++) if (servers[j] == servers[i]) dup = 1;
        if (dup) continue;
        if (!dns_query_one(host, servers[i], ip_out)) return 0;
    }
    return -1;
}

int net_ping(uint32_t dst, int *ticks_out) {
    if (!net_ready()) return -1;
    uint8_t p[40];
    memset(p, 0, sizeof(p));
    p[0] = 8;
    put16(p + 4, 0x4701);
    uint16_t seq = (uint16_t)(timer_ticks() & 0xFFFF);
    put16(p + 6, seq);
    memcpy(p + 8, "GeometricalOS", 14);
    put16(p + 2, checksum(p, 22, 0));
    ping_wait_ip = dst;
    ping_wait_seq = seq;
    ping_got = 0;
    ping_tick = timer_ticks();
    if (ip_send(dst, 1, p, 22)) return -1;
    uint32_t start = timer_ticks();
    while (!ping_got && timer_ticks() - start < 100) net_service();
    if (!ping_got) return -1;
    if (ticks_out) *ticks_out = (int)(timer_ticks() - ping_tick);
    return 0;
}

static int tcp_connect(uint32_t dst, uint16_t port) {
    memset(&tcp, 0, sizeof(tcp));
    if (arp_resolve(((dst ^ ip_addr) & ip_mask) ? ip_gw : dst, tcp.rmac)) return -1;
    tcp.rip = dst;
    tcp.rport = port;
    tcp.lport = (uint16_t)(40000u + (timer_ticks() & 0xFFF));
    tcp.snd_una = timer_ticks() ^ 0x12340000u;
    tcp.snd_nxt = tcp.snd_una;
    tcp.rcv_nxt = 0;
    tcp.state = TCP_SYN;
    tcp_send_raw(0x02, 0, 0, 1);
    tcp.snd_nxt += 1;
    uint32_t start = timer_ticks();
    while (tcp.state == TCP_SYN && timer_ticks() - start < 200) net_service();
    return tcp.state == TCP_EST ? 0 : -1;
}

static int tcp_send_data(const void *data, int len) {
    const uint8_t *p = data;
    while (len > 0) {
        int chunk = len > 1000 ? 1000 : len;
        tcp_send_raw(0x18, p, chunk, 1);
        uint32_t expect = tcp.snd_nxt + (uint32_t)chunk;
        tcp.snd_nxt = expect;
        uint32_t start = timer_ticks();
        while (tcp.snd_una < expect && tcp.state != TCP_CLOSED && timer_ticks() - start < 200)
            net_service();
        if (tcp.snd_una < expect || tcp.state == TCP_CLOSED) return -1;
        p += chunk;
        len -= chunk;
    }
    return 0;
}

static int tcp_recv_until(uint8_t *buf, int max, int *out) {
    tcp.recv = buf;
    tcp.recv_max = max;
    tcp.recv_len = 0;
    tcp.fin_seen = 0;
    uint32_t quiet = timer_ticks();
    int last = 0;
    while (timer_ticks() - quiet < 200 && tcp.recv_len < max && tcp.state != TCP_CLOSED) {
        net_service();
        if (tcp.recv_len != last) { last = tcp.recv_len; quiet = timer_ticks(); }
        if (tcp.fin_seen && tcp.recv_len == last) break;
    }
    *out = tcp.recv_len;
    tcp_send_raw(0x11, 0, 0, 0);
    tcp.snd_nxt += 1;
    tcp.state = TCP_CLOSED;
    return 0;
}

static void (*http_progress)(const char *);
void http_set_progress(void (*fn)(const char *msg)) { http_progress = fn; }
static void progress(const char *s) { if (http_progress) http_progress(s); }

static int header_has(const uint8_t *hdr, int hdr_end, const char *key) {
    int klen = (int)strlen(key);
    for (int i = 0; i + klen + 1 < hdr_end; i++) {
        if ((i == 0 || hdr[i - 1] == '\n') && strncasecmp((const char *)hdr + i, key, (size_t)klen) == 0)
            return 1;
    }
    return 0;
}

static int decode_chunked(uint8_t *src, int len, uint8_t *dst, int max) {
    int i = 0, o = 0;
    while (i < len && o < max) {
        int size = 0, any = 0;
        while (i < len && src[i] != '\n') {
            char c = (char)src[i++];
            if (c == '\r' || c == ';') continue;
            any = 1;
            if (c >= '0' && c <= '9') size = (size << 4) + (c - '0');
            else if (c >= 'a' && c <= 'f') size = (size << 4) + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') size = (size << 4) + (c - 'A' + 10);
        }
        if (i < len && src[i] == '\n') i++;
        if (!any) break;
        if (size == 0) break;
        if (i + size > len) size = len - i;
        if (o + size > max) size = max - o;
        memcpy(dst + o, src + i, (size_t)size);
        o += size;
        i += size;
        if (i < len && src[i] == '\r') i++;
        if (i < len && src[i] == '\n') i++;
    }
    return o;
}

static int url_split(const char *url, char *host, int hostmax, uint16_t *port, const char **path) {
    if (strncmp(url, "http://", 7) != 0) return -1;
    url += 7;
    int i = 0;
    while (url[i] && url[i] != '/' && url[i] != ':' && i + 1 < hostmax) {
        host[i] = url[i];
        i++;
    }
    host[i] = 0;
    if (!host[0]) return -1;
    *port = 80;
    if (url[i] == ':') {
        i++;
        int v = 0;
        if (url[i] < '0' || url[i] > '9') return -1;
        while (url[i] >= '0' && url[i] <= '9') {
            v = v * 10 + url[i] - '0';
            if (v > 65535) return -1;
            i++;
        }
        *port = (uint16_t)v;
    }
    *path = url[i] ? url + i : "/";
    if (**path != '/') return -1;
    return 0;
}

int http_get(const char *url, char *body, int max, int *out_len, int *status,
             char *final_url, int url_max) {
    char current[160];
    strncpy(current, url, sizeof(current));
    current[sizeof(current) - 1] = 0;
    if (status) *status = 0;
    if (out_len) *out_len = 0;
    if (!net_ready() && net_init()) return -1;
    for (int hop = 0; hop < 5; hop++) {
        if (strncmp(current, "https://", 8) == 0) return -2;
        char host[80];
        uint16_t port = 80;
        const char *path = "/";
        if (url_split(current, host, sizeof(host), &port, &path)) return -3;
        progress("Resolving...");
        uint32_t dip = 0;
        if (net_resolve(host, &dip)) return -4;
        progress("Connecting...");
        if (tcp_connect(dip, port)) return -5;
        progress("Loading...");
        char req[512];
        int n = 0;
        const char *p0 = "GET ";
        const char *p1 = path;
        const char *p2 = " HTTP/1.0\r\nHost: ";
        const char *p3 = host;
        for (const char *s = p0; *s && n + 1 < (int)sizeof(req); s++) req[n++] = *s;
        for (const char *s = p1; *s && n + 1 < (int)sizeof(req); s++) req[n++] = *s;
        for (const char *s = p2; *s && n + 1 < (int)sizeof(req); s++) req[n++] = *s;
        for (const char *s = p3; *s && n + 1 < (int)sizeof(req); s++) req[n++] = *s;
        if (port != 80 && n + 8 < (int)sizeof(req)) {
            req[n++] = ':';
            char rev[8]; int r = 0, v = port;
            if (!v) rev[r++] = '0';
            while (v && r < 8) { rev[r++] = (char)('0' + v % 10); v /= 10; }
            while (r) req[n++] = rev[--r];
        }
        const char *tail = "\r\nUser-Agent: GeometricalOS\r\nAccept: text/html,*/*\r\nConnection: close\r\n\r\n";
        for (const char *s = tail; *s && n + 1 < (int)sizeof(req); s++) req[n++] = *s;
        if (tcp_send_data(req, n)) { tcp.state = TCP_CLOSED; return -6; }
        static uint8_t *raw;
        if (!raw) raw = (uint8_t *)0x780000;
        int got = 0;
        tcp_recv_until(raw, 80 * 1024 - 1, &got);
        raw[got] = 0;
        int hdr = 0;
        for (int i = 0; i + 3 < got; i++) {
            if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') {
                hdr = i + 4;
                break;
            }
        }
        if (!hdr) return -7;
        int code = 0;
        const char *sp = (const char *)raw;
        while (*sp && *sp != ' ') sp++;
        if (*sp == ' ') code = 0;
        while (*sp == ' ') sp++;
        while (*sp >= '0' && *sp <= '9') code = code * 10 + (*sp++ - '0');
        if (status) *status = code;
        char location[160];
        location[0] = 0;
        for (int i = 0; i + 10 < hdr; i++) {
            if ((raw[i] == 'L' || raw[i] == 'l') && (raw[i + 1] == 'o' || raw[i + 1] == 'O')) {
                const char *line = (const char *)raw + i;
                if (strncasecmp(line, "location:", 9) == 0) {
                    line += 9;
                    while (*line == ' ') line++;
                    int k = 0;
                    while (line[k] && line[k] != '\r' && line[k] != '\n' && k + 1 < (int)sizeof(location)) {
                        location[k] = line[k];
                        k++;
                    }
                    location[k] = 0;
                    break;
                }
            }
        }
        int blen = got - hdr;
        if (blen < 0) blen = 0;
        if (header_has(raw, hdr, "transfer-encoding: chunked")) {
            static uint8_t *unchunk;
            if (!unchunk) unchunk = (uint8_t *)0x7A0000;
            blen = decode_chunked(raw + hdr, blen, unchunk, 64 * 1024);
            if (blen > max - 1) blen = max - 1;
            memcpy(body, unchunk, (size_t)blen);
        } else {
            if (blen > max - 1) blen = max - 1;
            memcpy(body, raw + hdr, (size_t)blen);
        }
        body[blen] = 0;
        if (out_len) *out_len = blen;
        if ((code == 301 || code == 302 || code == 303 || code == 307) && location[0]) {
            if (strncmp(location, "http://", 7) == 0 || strncmp(location, "https://", 8) == 0) {
                strncpy(current, location, sizeof(current));
                current[sizeof(current) - 1] = 0;
            } else if (location[0] == '/') {
                char next[160];
                int o = 0;
                const char *prefix = "http://";
                for (const char *s = prefix; *s && o + 1 < (int)sizeof(next); s++) next[o++] = *s;
                for (int i = 0; host[i] && o + 1 < (int)sizeof(next); i++) next[o++] = host[i];
                if (port != 80) {
                    next[o++] = ':';
                    char num[8]; int np = 0; int v = port;
                    char rev[8]; int r = 0;
                    if (!v) rev[r++] = '0';
                    while (v) { rev[r++] = (char)('0' + v % 10); v /= 10; }
                    while (r) num[np++] = rev[--r];
                    num[np] = 0;
                    for (int i = 0; num[i] && o + 1 < (int)sizeof(next); i++) next[o++] = num[i];
                }
                for (int i = 0; location[i] && o + 1 < (int)sizeof(next); i++) next[o++] = location[i];
                next[o] = 0;
                strncpy(current, next, sizeof(current));
            } else {
                return -8;
            }
            continue;
        }
        if (final_url && url_max > 0) {
            strncpy(final_url, current, (size_t)url_max);
            final_url[url_max - 1] = 0;
        }
        serial_write("HTTP: ");
        serial_dec((uint32_t)code);
        serial_write(" bytes ");
        serial_dec((uint32_t)blen);
        serial_write("\n");
        return code >= 200 && code < 400 ? 0 : -9;
    }
    return -10;
}
