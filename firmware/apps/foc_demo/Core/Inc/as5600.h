#ifndef __AS5600_H
#define __AS5600_H

#include <stdint.h>

/* AS5600 on two GPIO-header pins, bit-banged I2C (open drain + pull-up).
 * as5600_detect() scans header Pin1..Pin7 for the SCL/SDA pair. */
int as5600_detect(void);
int as5600_found(void);
const char *as5600_scl_name(void);
const char *as5600_sda_name(void);

/* 12-bit RAW ANGLE, 0..4095. Safe to call from the control interrupt only. */
int as5600_read_raw(uint16_t *raw);
/* Diagnostics; call only while the control loop is not reading. */
int as5600_read_diag(uint8_t *status, uint8_t *agc, uint16_t *magnitude, uint16_t *conf);
uint32_t as5600_read_time_us(void);
int as5600_fast_mode(void);

#endif /* __AS5600_H */
