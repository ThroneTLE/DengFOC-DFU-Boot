#ifndef __DRV8303_H
#define __DRV8303_H

#include <stdint.h>

void drv_spi_init(void);
int drv_read(uint8_t reg, uint16_t *data);
int drv_write(uint8_t reg, uint16_t data);
/* 6-PWM mode, OC latch shutdown at the lowest Vds threshold, gain 80.
 * Returns 1 when the registers read back as written. */
int drv_configure(void);

#endif /* __DRV8303_H */
