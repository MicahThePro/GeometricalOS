#ifndef GOS_NET_H
#define GOS_NET_H

#include <stdint.h>

typedef struct {
    void (*connected)(int connection, void *context);
    void (*data)(int connection, const uint8_t *bytes, uint32_t length,
                 void *context);
    void (*closed)(int connection, void *context);
} NetTcpEvents;

int net_start(void);
void net_stop(void);
int net_ready(void);
void net_get_ipv4(uint8_t address[4]);
int net_tcp_listen(uint16_t port, const NetTcpEvents *events, void *context);
void net_tcp_unlisten(uint16_t port);
int net_tcp_send(int connection, const void *bytes, uint32_t length);
int net_tcp_close(int connection);
int net_tcp_active(int connection);
int net_tcp_writable(int connection);
int net_tcp_peer_ipv4(int connection, uint8_t address[4]);
int net_tcp_received_fin(int connection);
void net_poll(void);

#endif
