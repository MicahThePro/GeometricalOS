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
/* USBSTS, CRCR, the first command's completion, and whether it timed out. */
const char *xhci_cmd_line(int bus, int dev, int fn);
const XhciHandoff *xhci_handoff_at(int index);
const char *xhci_status_line(void);
void xhci_set_status_line(const char *text);
void xhci_append_status(const char *extra);
uint32_t xhci_kbd_reports(void);
uint32_t xhci_mouse_reports(void);
uint32_t xhci_hid_errors(void);
int xhci_hid_last_error(void);
/* Marks the 2 MB page holding phys uncached in the kernel page tables. */
void xhci_force_uc(uint32_t phys);
void *xhci_heap_alloc(uint32_t size);

#endif
