#include "net.h"
#include "pcnet.h"
#include "input.h"
#include "serial.h"
#include "io.h"

#define NET_ETHERNET_HEADER 14u
#define NET_IPV4_HEADER     20u
#define NET_TCP_HEADER      20u
#define NET_TCP_MSS         512u
#define NET_FRAME_MAX       (NET_ETHERNET_HEADER + NET_IPV4_HEADER + NET_TCP_HEADER + NET_TCP_MSS)
#define NET_LISTENER_COUNT  4
#define NET_CONNECTION_COUNT 8
#define NET_RETRY_TICKS     50u
#define NET_RETRY_LIMIT     6u
#define NET_IDLE_TICKS      6000u
#define NET_TIME_WAIT_TICKS 200u

enum {
    TCP_FIN = 0x01,
    TCP_SYN = 0x02,
    TCP_RST = 0x04,
    TCP_PSH = 0x08,
    TCP_ACK = 0x10
};

typedef struct {
    uint16_t port;
    const NetTcpEvents *events;
    void *context;
    int active;
} NetListener;

typedef struct {
    uint32_t seq;
    uint32_t end_seq;
    uint32_t sent_at;
    uint16_t length;
    uint8_t flags;
    uint8_t retries;
    int active;
    int sent;
    uint8_t payload[NET_TCP_MSS];
} NetPending;

typedef struct {
    uint32_t remote_ip;
    uint32_t receive_next;
    uint32_t send_next;
    uint32_t last_activity;
    uint32_t release_at;
    uint16_t remote_port;
    uint16_t local_port;
    uint8_t remote_mac[6];
    const NetTcpEvents *events;
    void *context;
    NetPending pending;
    int used;
    int connected;
    int close_requested;
    int remote_closed;
    int local_fin_acked;
    int close_notified;
} NetConnection;

static NetListener listeners[NET_LISTENER_COUNT];
static NetConnection connections[NET_CONNECTION_COUNT];
static uint8_t local_ip[4] = {192, 168, 56, 2};
static uint8_t tx_frame[NET_FRAME_MAX];
static uint16_t ip_id;
static uint32_t initial_sequence = 0x47534F53u;
static int started;

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void write_be16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void write_be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t checksum_add(const uint8_t *bytes, uint32_t length, uint32_t sum) {
    while (length >= 2) {
        sum += ((uint32_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        length -= 2;
    }
    if (length) sum += (uint32_t)bytes[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return sum;
}

static uint16_t checksum_finish(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t ipv4_checksum(const uint8_t *header, uint32_t length) {
    return checksum_finish(checksum_add(header, length, 0));
}

static uint16_t tcp_checksum(const uint8_t *ip, const uint8_t *tcp,
                             uint16_t length) {
    uint32_t sum = checksum_add(ip + 12, 8, 0);
    sum += 6;
    sum += length;
    sum = checksum_add(tcp, length, sum);
    return checksum_finish(sum);
}

static uint32_t local_ip_value(void) {
    return ((uint32_t)local_ip[0] << 24) | ((uint32_t)local_ip[1] << 16) |
           ((uint32_t)local_ip[2] << 8) | local_ip[3];
}

void net_get_ipv4(uint8_t address[4]) {
    if (!address) return;
    memcpy(address, local_ip, 4);
}

int net_ready(void) {
    return started && pcnet_ready();
}

int net_start(void) {
    if (net_ready()) return 0;
    memset(listeners, 0, sizeof(listeners));
    memset(connections, 0, sizeof(connections));
    int result = pcnet_init();
    if (result) {
        started = 0;
        return result;
    }
    started = 1;
    serial_write("NET: static IPv4 192.168.56.2\n");
    return 0;
}

void net_stop(void) {
    started = 0;
    memset(listeners, 0, sizeof(listeners));
    for (int i = 0; i < NET_CONNECTION_COUNT; i++) {
        NetConnection *conn = &connections[i];
        if (conn->used && conn->events && conn->events->closed &&
            !conn->close_notified)
            conn->events->closed(i + 1, conn->context);
    }
    memset(connections, 0, sizeof(connections));
    pcnet_stop();
}

int net_tcp_listen(uint16_t port, const NetTcpEvents *events, void *context) {
    if (!net_ready() || !port || !events) return -1;
    for (int i = 0; i < NET_LISTENER_COUNT; i++) {
        if (listeners[i].active && listeners[i].port == port) return -2;
    }
    for (int i = 0; i < NET_LISTENER_COUNT; i++) {
        if (!listeners[i].active) {
            listeners[i].port = port;
            listeners[i].events = events;
            listeners[i].context = context;
            listeners[i].active = 1;
            return 0;
        }
    }
    return -3;
}

void net_tcp_unlisten(uint16_t port) {
    for (int i = 0; i < NET_LISTENER_COUNT; i++)
        if (listeners[i].active && listeners[i].port == port)
            memset(&listeners[i], 0, sizeof(listeners[i]));
}

int net_tcp_active(int connection) {
    int index = connection - 1;
    return index >= 0 && index < NET_CONNECTION_COUNT &&
           connections[index].used && connections[index].connected;
}

int net_tcp_writable(int connection) {
    int index = connection - 1;
    if (index < 0 || index >= NET_CONNECTION_COUNT) return 0;
    NetConnection *conn = &connections[index];
    return conn->used && conn->connected && !conn->close_requested &&
           !conn->remote_closed && !conn->pending.active;
}

int net_tcp_peer_ipv4(int connection, uint8_t address[4]) {
    int index = connection - 1;
    if (!address || index < 0 || index >= NET_CONNECTION_COUNT ||
        !connections[index].used)
        return -1;
    write_be32(address, connections[index].remote_ip);
    return 0;
}

int net_tcp_received_fin(int connection) {
    int index = connection - 1;
    return index >= 0 && index < NET_CONNECTION_COUNT &&
           connections[index].used && connections[index].remote_closed;
}

static int seq_after(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

static int send_tcp_packet(uint32_t remote_ip, const uint8_t remote_mac[6],
                           uint16_t local_port, uint16_t remote_port,
                           uint32_t sequence, uint32_t acknowledgement,
                           uint8_t flags, const uint8_t *payload,
                           uint16_t payload_length) {
    if (payload_length > NET_TCP_MSS || !pcnet_ready()) return -1;
    uint8_t *ip = tx_frame + NET_ETHERNET_HEADER;
    uint8_t *tcp = ip + NET_IPV4_HEADER;
    const uint8_t *local_mac = pcnet_mac();
    if (!local_mac) return -1;

    memcpy(tx_frame, remote_mac, 6);
    memcpy(tx_frame + 6, local_mac, 6);
    write_be16(tx_frame + 12, 0x0800u);

    memset(ip, 0, NET_IPV4_HEADER + NET_TCP_HEADER);
    ip[0] = 0x45;
    write_be16(ip + 2, (uint16_t)(NET_IPV4_HEADER + NET_TCP_HEADER + payload_length));
    write_be16(ip + 4, ++ip_id);
    ip[8] = 64;
    ip[9] = 6;
    memcpy(ip + 12, local_ip, 4);
    write_be32(ip + 16, remote_ip);
    write_be16(ip + 10, ipv4_checksum(ip, NET_IPV4_HEADER));

    write_be16(tcp, local_port);
    write_be16(tcp + 2, remote_port);
    write_be32(tcp + 4, sequence);
    write_be32(tcp + 8, acknowledgement);
    tcp[12] = 0x50;
    tcp[13] = flags;
    write_be16(tcp + 14, 8192u);
    if (payload_length) memcpy(tcp + NET_TCP_HEADER, payload, payload_length);
    write_be16(tcp + 16, tcp_checksum(ip, tcp,
                 (uint16_t)(NET_TCP_HEADER + payload_length)));

    return pcnet_send(tx_frame,
                      NET_ETHERNET_HEADER + NET_IPV4_HEADER + NET_TCP_HEADER +
                      payload_length);
}

static void send_ack(const NetConnection *conn) {
    (void)send_tcp_packet(conn->remote_ip, conn->remote_mac, conn->local_port,
                          conn->remote_port, conn->send_next,
                          conn->receive_next, TCP_ACK, 0, 0);
}

static void notify_closed(int index) {
    NetConnection *conn = &connections[index];
    if (conn->close_notified) return;
    conn->close_notified = 1;
    if (conn->events && conn->events->closed)
        conn->events->closed(index + 1, conn->context);
}

static void discard_connection(int index, int notify) {
    if (notify) notify_closed(index);
    memset(&connections[index], 0, sizeof(connections[index]));
}

static void queue_pending(NetConnection *conn, uint8_t flags,
                          const uint8_t *payload, uint16_t length) {
    NetPending *pending = &conn->pending;
    pending->seq = conn->send_next;
    pending->length = length;
    pending->flags = flags;
    pending->retries = 0;
    pending->sent = 0;
    pending->active = 1;
    if (length) memcpy(pending->payload, payload, length);
    conn->send_next += length;
    if (flags & (TCP_SYN | TCP_FIN)) conn->send_next++;
    pending->end_seq = conn->send_next;
}

int net_tcp_send(int connection, const void *bytes, uint32_t length) {
    int index = connection - 1;
    if (index < 0 || index >= NET_CONNECTION_COUNT || (!bytes && length) ||
        length > NET_TCP_MSS)
        return -1;
    NetConnection *conn = &connections[index];
    if (!conn->used || !conn->connected || conn->close_requested ||
        conn->remote_closed || conn->pending.active)
        return -2;
    queue_pending(conn, TCP_ACK | TCP_PSH, bytes, (uint16_t)length);
    return 0;
}

int net_tcp_close(int connection) {
    int index = connection - 1;
    if (index < 0 || index >= NET_CONNECTION_COUNT) return -1;
    NetConnection *conn = &connections[index];
    if (!conn->used) return -1;
    conn->close_requested = 1;
    return 0;
}

static int find_connection(uint32_t remote_ip, uint16_t remote_port,
                           uint16_t local_port) {
    for (int i = 0; i < NET_CONNECTION_COUNT; i++) {
        NetConnection *conn = &connections[i];
        if (conn->used && conn->remote_ip == remote_ip &&
            conn->remote_port == remote_port && conn->local_port == local_port)
            return i;
    }
    return -1;
}

static NetListener *find_listener(uint16_t port) {
    for (int i = 0; i < NET_LISTENER_COUNT; i++)
        if (listeners[i].active && listeners[i].port == port)
            return &listeners[i];
    return 0;
}

static int new_connection(uint32_t remote_ip, uint16_t remote_port,
                          uint16_t local_port, const uint8_t remote_mac[6],
                          uint32_t remote_sequence, NetListener *listener) {
    for (int i = 0; i < NET_CONNECTION_COUNT; i++) {
        NetConnection *conn = &connections[i];
        if (conn->used) continue;
        memset(conn, 0, sizeof(*conn));
        conn->used = 1;
        conn->remote_ip = remote_ip;
        conn->remote_port = remote_port;
        conn->local_port = local_port;
        memcpy(conn->remote_mac, remote_mac, 6);
        conn->receive_next = remote_sequence + 1;
        conn->send_next = initial_sequence += 0x101u;
        conn->events = listener->events;
        conn->context = listener->context;
        conn->last_activity = timer_ticks();
        queue_pending(conn, TCP_SYN | TCP_ACK, 0, 0);
        return i;
    }
    return -1;
}

static void send_reset(uint32_t remote_ip, const uint8_t remote_mac[6],
                       uint16_t local_port, uint16_t remote_port,
                       uint32_t sequence, uint32_t acknowledgement,
                       uint8_t incoming_flags, uint16_t payload_length) {
    if (incoming_flags & TCP_ACK) {
        (void)send_tcp_packet(remote_ip, remote_mac, local_port, remote_port,
                              acknowledgement, 0, TCP_RST, 0, 0);
    } else {
        uint32_t advance = payload_length;
        if (incoming_flags & TCP_SYN) advance++;
        if (incoming_flags & TCP_FIN) advance++;
        (void)send_tcp_packet(remote_ip, remote_mac, local_port, remote_port,
                              0, sequence + advance, TCP_RST | TCP_ACK, 0, 0);
    }
}

static void receive_tcp(const uint8_t *ip, uint32_t ip_length,
                        const uint8_t remote_mac[6]) {
    if (ip_length < NET_IPV4_HEADER + NET_TCP_HEADER) return;
    uint32_t ihl = (uint32_t)(ip[0] & 0x0Fu) * 4u;
    uint32_t total = read_be16(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < NET_IPV4_HEADER ||
        total < ihl + NET_TCP_HEADER || total > ip_length ||
        ipv4_checksum(ip, ihl) != 0 ||
        (read_be16(ip + 6) & 0x3FFFu) != 0 ||
        read_be32(ip + 16) != local_ip_value())
        return;

    const uint8_t *tcp = ip + ihl;
    uint32_t tcp_length = total - ihl;
    uint32_t tcp_header = (uint32_t)(tcp[12] >> 4) * 4u;
    if (tcp_header < NET_TCP_HEADER || tcp_header > tcp_length ||
        tcp_checksum(ip, tcp, (uint16_t)tcp_length) != 0)
        return;

    uint16_t remote_port = read_be16(tcp);
    uint16_t local_port = read_be16(tcp + 2);
    uint32_t sequence = read_be32(tcp + 4);
    uint32_t acknowledgement = read_be32(tcp + 8);
    uint8_t flags = tcp[13];
    const uint8_t *payload = tcp + tcp_header;
    uint16_t payload_length = (uint16_t)(tcp_length - tcp_header);
    uint32_t remote_ip = read_be32(ip + 12);
    int index = find_connection(remote_ip, remote_port, local_port);

    if (index < 0) {
        if (!(flags & TCP_SYN) || (flags & (TCP_ACK | TCP_RST))) {
            if (!(flags & TCP_RST))
                send_reset(remote_ip, remote_mac, local_port, remote_port,
                           sequence, acknowledgement, flags, payload_length);
            return;
        }
        NetListener *listener = find_listener(local_port);
        if (!listener) {
            send_reset(remote_ip, remote_mac, local_port, remote_port,
                       sequence, acknowledgement, flags, payload_length);
            return;
        }
        index = new_connection(remote_ip, remote_port, local_port, remote_mac,
                               sequence, listener);
        if (index < 0)
            send_reset(remote_ip, remote_mac, local_port, remote_port,
                       sequence, acknowledgement, flags, payload_length);
        return;
    }

    NetConnection *conn = &connections[index];
    conn->last_activity = timer_ticks();
    memcpy(conn->remote_mac, remote_mac, 6);
    if (flags & TCP_RST) {
        discard_connection(index, 1);
        return;
    }

    if (conn->pending.active && (flags & TCP_ACK) &&
        acknowledgement == conn->pending.end_seq) {
        uint8_t completed_flags = conn->pending.flags;
        conn->pending.active = 0;
        conn->pending.sent = 0;
        if (completed_flags & TCP_SYN) {
            conn->connected = 1;
            if (conn->events && conn->events->connected)
                conn->events->connected(index + 1, conn->context);
        }
        if (completed_flags & TCP_FIN) {
            conn->local_fin_acked = 1;
            if (conn->remote_closed)
                conn->release_at = timer_ticks() + NET_TIME_WAIT_TICKS;
        }
    }

    if (!conn->connected) return;
    if (sequence != conn->receive_next) {
        if (seq_after(sequence, conn->receive_next) ||
            seq_after(conn->receive_next, sequence))
            send_ack(conn);
        return;
    }

    if (payload_length) {
        conn->receive_next += payload_length;
        if (conn->events && conn->events->data)
            conn->events->data(index + 1, payload, payload_length, conn->context);
        send_ack(conn);
    }
    if ((flags & TCP_FIN) && !conn->remote_closed) {
        conn->receive_next++;
        conn->remote_closed = 1;
        if (conn->events && conn->events->closed)
            notify_closed(index);
        send_ack(conn);
        if (conn->local_fin_acked)
            conn->release_at = timer_ticks() + NET_TIME_WAIT_TICKS;
    }
}

static void receive_arp(const uint8_t *frame, uint32_t length) {
    if (length < NET_ETHERNET_HEADER + 28u) return;
    const uint8_t *arp = frame + NET_ETHERNET_HEADER;
    if (read_be16(arp) != 1 || read_be16(arp + 2) != 0x0800u ||
        arp[4] != 6 || arp[5] != 4 || read_be16(arp + 6) != 1 ||
        memcmp(arp + 24, local_ip, 4) != 0)
        return;

    uint8_t reply[NET_ETHERNET_HEADER + 28u];
    const uint8_t *local_mac = pcnet_mac();
    if (!local_mac) return;
    memcpy(reply, frame + 6, 6);
    memcpy(reply + 6, local_mac, 6);
    write_be16(reply + 12, 0x0806u);
    uint8_t *out = reply + NET_ETHERNET_HEADER;
    write_be16(out, 1);
    write_be16(out + 2, 0x0800u);
    out[4] = 6;
    out[5] = 4;
    write_be16(out + 6, 2);
    memcpy(out + 8, local_mac, 6);
    memcpy(out + 14, local_ip, 4);
    memcpy(out + 18, arp + 8, 6);
    memcpy(out + 24, arp + 14, 4);
    (void)pcnet_send(reply, sizeof(reply));
}

static void receive_frame(const uint8_t *frame, uint32_t length) {
    if (length < NET_ETHERNET_HEADER) return;
    uint16_t type = read_be16(frame + 12);
    if (type == 0x0806u) {
        receive_arp(frame, length);
    } else if (type == 0x0800u && length >= NET_ETHERNET_HEADER + NET_IPV4_HEADER) {
        const uint8_t *ip = frame + NET_ETHERNET_HEADER;
        if (ip[9] == 6)
            receive_tcp(ip, length - NET_ETHERNET_HEADER, frame + 6);
    }
}

static void service_connection(int index, uint32_t now) {
    NetConnection *conn = &connections[index];
    if (!conn->used) return;
    if (now - conn->last_activity > NET_IDLE_TICKS) {
        discard_connection(index, 1);
        return;
    }
    if (conn->release_at && (int32_t)(now - conn->release_at) >= 0) {
        discard_connection(index, 0);
        return;
    }

    NetPending *pending = &conn->pending;
    if (pending->active) {
        if (pending->sent && now - pending->sent_at < NET_RETRY_TICKS) return;
        if (pending->retries >= NET_RETRY_LIMIT) {
            discard_connection(index, 1);
            return;
        }
        if (send_tcp_packet(conn->remote_ip, conn->remote_mac, conn->local_port,
                            conn->remote_port, pending->seq, conn->receive_next,
                            pending->flags, pending->payload, pending->length) == 0) {
            pending->sent = 1;
            pending->sent_at = now;
            pending->retries++;
        }
        return;
    }
    if (conn->close_requested && conn->connected && !conn->local_fin_acked) {
        queue_pending(conn, TCP_FIN | TCP_ACK, 0, 0);
        service_connection(index, now);
    }
}

void net_poll(void) {
    if (!net_ready()) return;
    pcnet_poll(receive_frame);
    uint32_t now = timer_ticks();
    for (int i = 0; i < NET_CONNECTION_COUNT; i++)
        service_connection(i, now);
}
