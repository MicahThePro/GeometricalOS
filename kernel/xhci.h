#ifndef GOS_XHCI_H
#define GOS_XHCI_H

#include <stdint.h>

void xhci_init(void);
void xhci_handoff_all(void);
uint32_t xhci_scratchpad_count(uint32_t hcs2);
void xhci_poll(void);

typedef struct {
    uint8_t bus, dev, fn;
    uint8_t have_leg;
    uint16_t vendor, device;
    uint32_t bar_lo, bar_hi;
    uint32_t leg_before, leg_after;
    uint32_t ctl_before, ctl_after;
    uint32_t scratch;
    uint32_t page_size;
    uint32_t scratch_addr;
} XhciHandoff;

int xhci_handoff_count(void);
int xhci_usb_count(void);
const char *xhci_usb_line(int index);
const XhciHandoff *xhci_handoff_at(int index);
const char *xhci_status_line(void);
void xhci_set_status_line(const char *text);
void xhci_append_status(const char *extra);

#endif
