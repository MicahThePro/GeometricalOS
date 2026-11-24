#ifndef GOS_I2CSCAN_H
#define GOS_I2CSCAN_H

#include <stdint.h>

/* One I2C-HID device found in the AML, with its I2cSerialBusV2 resources. */
#define I2C_MAX_CAND 8
#define I2C_MAX_RES 4

typedef struct {
    char name[5];
    int score;
    int n_res;
    uint16_t addr[I2C_MAX_RES];
    uint32_t speed[I2C_MAX_RES];
    char ctl[I2C_MAX_RES][5];
    int gpio;
    uint8_t gpio_flags;
    int dsm;
} I2cCand;

int i2cscan_find(I2cCand *out, int cap, int *tables);
uint32_t i2cscan_mmio(const char *name, int *is_amd);

#endif
