#ifndef GOS_PCI_H
#define GOS_PCI_H

#include <stdint.h>

uint32_t pci_read(int bus, int dev, int fn, int off);
void pci_write(int bus, int dev, int fn, int off, uint32_t val);
void pci_write16(int bus, int dev, int fn, int off, uint16_t val);
int pci_find(uint16_t vendor, uint16_t device, int *bus, int *dev, int *fn);
int pci_find_class(uint8_t class, uint8_t sub, uint8_t prog, int *bus, int *dev, int *fn);

#endif
