#include "vmm.h"
#include "pci.h"
#include "io.h"
#include "serial.h"

/* VirtualBox VMMDev (PCI 80EE:CAFE). The request header matches the guest
 * additions layout used since VirtualBox 4: 24 bytes, version 0x10001.
 * Writing the physical address of a request to I/O offset 0 runs it.
 */
#define VMM_VENDOR 0x80EE
#define VMM_DEVICE 0xCAFE
#define VMM_HDR_VERSION 0x10001u
#define VMM_REQ_GET_MOUSE 1u
#define VMM_REQ_SET_MOUSE 2u
#define VMM_REQ_GUEST_INFO 50u
#define VMM_MOUSE_CAN_ABS 0x00000001u
#define VMM_MOUSE_HOST_ABS 0x00000002u
#define VMM_MOUSE_NEW 0x00000010u

struct vmm_hdr {
    uint32_t size;
    uint32_t version;
    uint32_t type;
    int32_t rc;
    uint32_t reserved1;
    uint32_t reserved2;
} __attribute__((packed));

struct vmm_mouse {
    struct vmm_hdr hdr;
    uint32_t features;
    int32_t x;
    int32_t y;
} __attribute__((packed));

struct vmm_info {
    struct vmm_hdr hdr;
    uint32_t interface_version;
    uint32_t os_type;
} __attribute__((packed));

static struct vmm_mouse req __attribute__((aligned(16)));
static struct vmm_info info __attribute__((aligned(16)));
static uint16_t io_port;
static volatile uint32_t *mmio;
static int present;
static int absolute;

static void vmm_submit(void *packet) {
    uint32_t phys = (uint32_t)(uintptr_t)packet;
    if (io_port) outl(io_port, phys);
    else if (mmio) mmio[0] = phys;
}

static int vmm_call_mouse(uint32_t type, uint32_t features) {
    memset(&req, 0, sizeof(req));
    req.hdr.size = sizeof(req);
    req.hdr.version = VMM_HDR_VERSION;
    req.hdr.type = type;
    req.hdr.rc = -1;
    req.features = features;
    vmm_submit(&req);
    return req.hdr.rc == 0;
}

int vmm_mouse_init(void) {
    int bus, dev, fn;
    present = 0;
    absolute = 0;
    io_port = 0;
    mmio = 0;
    if (pci_find(VMM_VENDOR, VMM_DEVICE, &bus, &dev, &fn)) return 0;
    uint32_t cmd = pci_read(bus, dev, fn, 4);
    uint32_t bar = pci_read(bus, dev, fn, 0x10);
    if (bar & 1u) {
        io_port = (uint16_t)(bar & ~3u);
        cmd |= 0x1u;
    } else if (bar & ~0xFu) {
        mmio = (volatile uint32_t *)(uintptr_t)(bar & ~0xFu);
        cmd |= 0x2u;
    } else {
        return 0;
    }
    pci_write(bus, dev, fn, 4, cmd | 0x4u);
    memset(&info, 0, sizeof(info));
    info.hdr.size = sizeof(info);
    info.hdr.version = VMM_HDR_VERSION;
    info.hdr.type = VMM_REQ_GUEST_INFO;
    info.hdr.rc = -1;
    info.interface_version = 0x00010004u;
    vmm_submit(&info);
    if (!vmm_call_mouse(VMM_REQ_SET_MOUSE, VMM_MOUSE_CAN_ABS | VMM_MOUSE_NEW)) {
        serial_write("MOUSE: vmmdev present, absolute request failed\n");
        return 0;
    }
    present = 1;
    if (vmm_call_mouse(VMM_REQ_GET_MOUSE, 0) && (req.features & VMM_MOUSE_HOST_ABS))
        absolute = 1;
    serial_write(absolute ? "MOUSE: absolute\n" : "MOUSE: vmmdev, ps/2 pointer\n");
    return absolute;
}

int vmm_mouse_absolute(void) { return absolute; }

int vmm_mouse_poll(int *x, int *y, int sw, int sh) {
    if (!present) return 0;
    if (!vmm_call_mouse(VMM_REQ_GET_MOUSE, 0)) return 0;
    if (req.features & VMM_MOUSE_HOST_ABS) absolute = 1;
    else {
        absolute = 0;
        return 0;
    }
    if (sw < 2 || sh < 2) return 0;
    int nx = (int)(((uint32_t)req.x * (uint32_t)(sw - 1)) / 65535u);
    int ny = (int)(((uint32_t)req.y * (uint32_t)(sh - 1)) / 65535u);
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx >= sw) nx = sw - 1;
    if (ny >= sh) ny = sh - 1;
    *x = nx;
    *y = ny;
    return 1;
}
