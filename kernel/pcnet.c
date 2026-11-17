#include "pcnet.h"
#include "pci.h"
#include "io.h"
#include "serial.h"

#define PCNET_VENDOR       0x1022u
#define PCNET_DEVICE       0x2000u
#define PCNET_RDP          0x10u
#define PCNET_RAP          0x12u
#define PCNET_RESET        0x14u
#define PCNET_BDP          0x16u
#define PCNET_RX_COUNT     4u
#define PCNET_TX_COUNT     2u
#define PCNET_BUFFER_SIZE  1024u
#define PCNET_OWN          0x8000u
#define PCNET_STP          0x0200u
#define PCNET_ENP          0x0100u
#define PCNET_ERR          0x4000u

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t buffer;
    uint16_t buffer_length;
    uint16_t status;
    uint32_t message_length;
    uint32_t reserved;
} PcnetDescriptor;

typedef struct __attribute__((packed)) {
    uint16_t mode;
    uint16_t ring_lengths;
    uint8_t mac[6];
    uint16_t reserved;
    uint32_t filter[2];
    uint32_t rx_ring;
    uint32_t tx_ring;
} PcnetInitBlock;

_Static_assert(sizeof(PcnetDescriptor) == 16, "PCnet descriptor size");
_Static_assert(sizeof(PcnetInitBlock) == 28, "PCnet init block size");

static PcnetInitBlock init_block __attribute__((aligned(16)));
static PcnetDescriptor rx_ring[PCNET_RX_COUNT] __attribute__((aligned(16)));
static PcnetDescriptor tx_ring[PCNET_TX_COUNT] __attribute__((aligned(16)));
static uint8_t rx_buffers[PCNET_RX_COUNT][PCNET_BUFFER_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buffers[PCNET_TX_COUNT][PCNET_BUFFER_SIZE] __attribute__((aligned(16)));
static uint8_t station_mac[6];
static uint16_t io_base;
static uint16_t last_init_csr0;
static uint16_t last_init_bcr20;
static uint16_t last_pci_command;
static uint32_t rx_index;
static uint32_t tx_index;
static int initialized;

static uint16_t csr_read(uint16_t index) {
    outw((uint16_t)(io_base + PCNET_RAP), index);
    return inw((uint16_t)(io_base + PCNET_RDP));
}

static void csr_write(uint16_t index, uint16_t value) {
    outw((uint16_t)(io_base + PCNET_RAP), index);
    outw((uint16_t)(io_base + PCNET_RDP), value);
}

static void bcr_write(uint16_t index, uint16_t value) {
    outw((uint16_t)(io_base + PCNET_RAP), index);
    outw((uint16_t)(io_base + PCNET_BDP), value);
}

static uint16_t bcr_read(uint16_t index) {
    outw((uint16_t)(io_base + PCNET_RAP), index);
    return inw((uint16_t)(io_base + PCNET_BDP));
}

static void memory_barrier(void) {
    __asm__ volatile ("" : : : "memory");
}

int pcnet_init(void) {
    int bus, device, function;
    initialized = 0;
    io_base = 0;
    last_init_csr0 = 0;
    last_init_bcr20 = 0;
    last_pci_command = 0;
    if (pci_find(PCNET_VENDOR, PCNET_DEVICE, &bus, &device, &function)) {
        serial_write("PCNET: VirtualBox PCnet adapter not found\n");
        return -1;
    }

    uint32_t bar = pci_read(bus, device, function, 0x10);
    if (!(bar & 1u)) {
        serial_write("PCNET: adapter has no I/O BAR\n");
        return -2;
    }
    io_base = (uint16_t)(bar & 0xFFFCu);
    if (io_base < 0x100u) {
        io_base = 0;
        serial_write("PCNET: invalid I/O base\n");
        return -2;
    }

    uint32_t command = pci_read(bus, device, function, 0x04);
    pci_write16(bus, device, function, 0x04, (uint16_t)(command | 0x0005u));
    last_pci_command = (uint16_t)pci_read(bus, device, function, 0x04);
    if ((last_pci_command & 0x0005u) != 0x0005u) {
        io_base = 0;
        serial_write("PCNET: PCI I/O or bus mastering could not be enabled\n");
        return -4;
    }

    (void)inw((uint16_t)(io_base + PCNET_RESET));
    csr_write(0, 0x0004u);
    bcr_write(20, 2);
    last_init_bcr20 = bcr_read(20);
    if ((last_init_bcr20 & 0x00FFu) != 2u) {
        last_init_csr0 = csr_read(0);
        serial_write("PCNET: software style readback BCR20=");
        serial_hex32(last_init_bcr20);
        serial_write(" CSR0=");
        serial_hex32(last_init_csr0);
        serial_write(" PCI_CMD=");
        serial_hex32(last_pci_command);
        serial_write("\n");
        io_base = 0;
        serial_write("PCNET: software style 2 could not be enabled\n");
        return -5;
    }

    for (int i = 0; i < 6; i++)
        station_mac[i] = inb((uint16_t)(io_base + (uint16_t)i));

    memset(&init_block, 0, sizeof(init_block));
    init_block.mode = 0;
    init_block.ring_lengths = (uint16_t)((2u << 4) | (1u << 12));
    memcpy(init_block.mac, station_mac, sizeof(station_mac));
    init_block.rx_ring = (uint32_t)(uintptr_t)rx_ring;
    init_block.tx_ring = (uint32_t)(uintptr_t)tx_ring;

    for (uint32_t i = 0; i < PCNET_RX_COUNT; i++) {
        rx_ring[i].buffer = (uint32_t)(uintptr_t)rx_buffers[i];
        rx_ring[i].buffer_length = (uint16_t)(0u - PCNET_BUFFER_SIZE);
        rx_ring[i].status = PCNET_OWN;
    }
    memset(tx_ring, 0, sizeof(tx_ring));

    uint32_t init_address = (uint32_t)(uintptr_t)&init_block;
    csr_write(1, (uint16_t)init_address);
    csr_write(2, (uint16_t)(init_address >> 16));
    csr_write(3, 0x0000u);
    csr_write(4, 0x0915u);
    memory_barrier();
    csr_write(0, 0x0001u);

    int ready = 0;
    for (uint32_t i = 0; i < 2000000u; i++) {
        last_init_csr0 = csr_read(0);
        if (last_init_csr0 & 0x0900u) {
            ready = 1;
            break;
        }
    }
    if (!ready || !(last_init_csr0 & 0x0100u) ||
        (last_init_csr0 & 0x0800u)) {
        if (!last_init_csr0) last_init_csr0 = csr_read(0);
        last_init_bcr20 = bcr_read(20);
        serial_write("PCNET: INIT failed CSR0=");
        serial_hex32(last_init_csr0);
        serial_write(" BCR20=");
        serial_hex32(last_init_bcr20);
        serial_write(" PCI_CMD=");
        serial_hex32(last_pci_command);
        serial_write("\n");
        csr_write(0, 0x0004u);
        io_base = 0;
        serial_write("PCNET: initialization timed out\n");
        return -3;
    }

    csr_write(0, 0x0100u);
    csr_write(0, 0x0002u);
    rx_index = 0;
    tx_index = 0;
    initialized = 1;
    serial_write("PCNET: ready\n");
    return 0;
}

int pcnet_ready(void) {
    return initialized;
}

uint16_t pcnet_last_init_csr0(void) {
    return last_init_csr0;
}

uint16_t pcnet_last_init_bcr20(void) {
    return last_init_bcr20;
}

uint16_t pcnet_last_pci_command(void) {
    return last_pci_command;
}

void pcnet_stop(void) {
    if (!initialized) return;
    csr_write(0, 0x0004u);
    initialized = 0;
    io_base = 0;
}

const uint8_t *pcnet_mac(void) {
    return initialized ? station_mac : 0;
}

int pcnet_send(const void *frame, uint32_t length) {
    if (!initialized || !frame || length < 14 || length > PCNET_BUFFER_SIZE)
        return -1;

    PcnetDescriptor *desc = &tx_ring[tx_index];
    if (desc->status & PCNET_OWN) return -2;
    memcpy(tx_buffers[tx_index], frame, length);
    desc->buffer = (uint32_t)(uintptr_t)tx_buffers[tx_index];
    desc->buffer_length = (uint16_t)(0u - length);
    desc->message_length = 0;
    desc->reserved = 0;
    memory_barrier();
    desc->status = PCNET_OWN | PCNET_STP | PCNET_ENP;
    memory_barrier();
    csr_write(0, 0x0008u);
    tx_index = (tx_index + 1) & (PCNET_TX_COUNT - 1);
    return 0;
}

void pcnet_poll(PcnetReceiveFn receive) {
    if (!initialized || !receive) return;

    for (uint32_t budget = 0; budget < PCNET_RX_COUNT; budget++) {
        PcnetDescriptor *desc = &rx_ring[rx_index];
        memory_barrier();
        if (desc->status & PCNET_OWN) break;

        uint16_t status = desc->status;
        uint32_t length = desc->message_length & 0x0FFFu;
        if (!(status & PCNET_ERR) &&
            (status & (PCNET_STP | PCNET_ENP)) == (PCNET_STP | PCNET_ENP) &&
            length >= 4u && length <= PCNET_BUFFER_SIZE) {
            receive(rx_buffers[rx_index], length - 4u);
        }

        desc->message_length = 0;
        desc->status = PCNET_OWN;
        memory_barrier();
        rx_index = (rx_index + 1) & (PCNET_RX_COUNT - 1);
    }
}
