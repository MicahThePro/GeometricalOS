#ifndef GOS_NET_H
#define GOS_NET_H

#include <stdint.h>

int net_init(void);
int net_ready(void);
void net_service(void);
void net_format_ip(uint32_t ip, char *dst, int n);

uint32_t net_ip(void);
uint32_t net_mask(void);
uint32_t net_gateway(void);
uint32_t net_dns(void);
const uint8_t *net_mac(void);

int net_resolve(const char *host, uint32_t *ip_out);
int net_ping(uint32_t ip, int *ticks_out);

/* Fetch a URL into body. Follows HTTP redirects. Returns 0 on a response. */
int http_get(const char *url, char *body, int max, int *out_len, int *status,
             char *final_url, int url_max);
void http_set_progress(void (*fn)(const char *msg));

#endif
