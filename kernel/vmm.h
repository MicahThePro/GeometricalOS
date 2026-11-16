#ifndef GOS_VMM_H
#define GOS_VMM_H

/* VirtualBox guest device. Returns 1 when the host is feeding absolute coordinates. */
int vmm_mouse_init(void);
int vmm_mouse_absolute(void);
/* Writes screen coordinates. Returns 1 if a new absolute position was applied. */
int vmm_mouse_poll(int *x, int *y, int sw, int sh);
/* Checks for and applies a host display resize request. */
int vmm_display_poll(void);

#endif
