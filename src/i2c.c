#include "i2c.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>

int i2c_open(const char *dev)
{
    return open(dev, O_RDWR | O_CLOEXEC);
}

static int transfer(int fd, struct i2c_msg *msgs, unsigned n)
{
    struct i2c_rdwr_ioctl_data data = { .msgs = msgs, .nmsgs = n };

    if (ioctl(fd, I2C_RDWR, &data) < 0)
        return -1;
    return 0;
}

int i2c_write(int fd, uint8_t addr, const uint8_t *buf, size_t len)
{
    if (len == 0 || len > UINT16_MAX) {
        errno = EINVAL;
        return -1;
    }
    struct i2c_msg msg = { .addr = addr, .flags = 0, .len = (uint16_t)len, .buf = (uint8_t *)buf };
    return transfer(fd, &msg, 1);
}

int i2c_read(int fd, uint8_t addr, uint8_t *buf, size_t len)
{
    if (len == 0 || len > UINT16_MAX) {
        errno = EINVAL;
        return -1;
    }
    struct i2c_msg msg = { .addr = addr, .flags = I2C_M_RD, .len = (uint16_t)len, .buf = buf };
    return transfer(fd, &msg, 1);
}

int i2c_write_read(int fd, uint8_t addr, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen)
{
    if (wlen == 0 || wlen > UINT16_MAX || rlen == 0 || rlen > UINT16_MAX) {
        errno = EINVAL;
        return -1;
    }
    struct i2c_msg msgs[2] = {
        { .addr = addr, .flags = 0, .len = (uint16_t)wlen, .buf = (uint8_t *)w },
        { .addr = addr, .flags = I2C_M_RD, .len = (uint16_t)rlen, .buf = r },
    };
    return transfer(fd, msgs, 2);
}
