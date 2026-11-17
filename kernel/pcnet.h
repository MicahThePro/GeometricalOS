#ifndef GOS_PCNET_H
#define GOS_PCNET_H

#include <stdint.h>

typedef void (*PcnetReceiveFn)(const uint8_t *frame, uint32_t length);

int pcnet_init(void);
void pcnet_stop(void);
int pcnet_ready(void);
uint16_t pcnet_last_init_csr0(void);
uint16_t pcnet_last_init_bcr20(void);
uint16_t pcnet_last_pci_command(void);
const uint8_t *pcnet_mac(void);
int pcnet_send(const void *frame, uint32_t length);
void pcnet_poll(PcnetReceiveFn receive);

#endif
