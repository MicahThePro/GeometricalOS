#ifndef GOS_ACPI_H
#define GOS_ACPI_H

#include <stdint.h>

void acpi_bringup(void);
int acpi_step_count(void);
const char *acpi_step_line(int index);

/* IRQ1 routing from the MADT. gsi is the IOAPIC pin. level/low are 0 when
 * the table says the ISA default (edge, active high). ioapic is 0 if none. */
int acpi_irq1(uint32_t *ioapic, uint32_t *gsi, int *level, int *low);

/* index-th table with this signature, or 0. "DSDT" comes from the FADT. */
const uint8_t *acpi_table(const char *sig, int index);

#endif
