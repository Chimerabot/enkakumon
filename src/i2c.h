/*
 * i2c.h — raw /dev/i2c-N access through ioctl(I2C_RDWR).
 *
 * All functions return 0 on success or -1 with errno set; callers decide
 * whether and how often to log.
 */
#ifndef ENKAKUMON_I2C_H
#define ENKAKUMON_I2C_H

#include <stddef.h>
#include <stdint.h>

/* Returns an O_CLOEXEC fd, or -1. */
int i2c_open(const char *dev);

int i2c_write(int fd, uint8_t addr, const uint8_t *buf, size_t len);
int i2c_read(int fd, uint8_t addr, uint8_t *buf, size_t len);

/* Write then read with a repeated start (register reads). */
int i2c_write_read(int fd, uint8_t addr, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen);

#endif
