/* GENI I2C, FIFO mode, polled: the speaker amps' bus (QUPv3 wrap0 SE4). 0 = ok, -1 = NACK, else error. */
#ifndef AUDIO_I2C_H
#define AUDIO_I2C_H
#include <stdint.h>
int i2c_init(void);
int i2c_write(uint8_t addr, const uint8_t *buf, unsigned n);
int i2c_write_read(uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn);
#endif
