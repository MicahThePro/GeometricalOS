#ifndef GOS_XHCI_H
#define GOS_XHCI_H

void xhci_init(void);
void xhci_poll(void);
const char *xhci_status_line(void);
void xhci_append_status(const char *extra);

#endif
