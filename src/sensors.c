#include "sensors.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "i2c.h"

#define SCD40_ADDR 0x62
#define SGP40_ADDR 0x59
#define APDS9960_ADDR 0x39
#define LSM6DSOX_ADDR 0x6A

/* SCD40 commands */
#define SCD40_START_PERIODIC 0x21B1
#define SCD40_STOP_PERIODIC 0x3F86
#define SCD40_DATA_READY 0xE4B8
#define SCD40_READ_MEASUREMENT 0xEC05

/* SGP40 commands */
#define SGP40_MEASURE_RAW 0x260F
#define SGP40_GET_SERIAL 0x3682

/* APDS9960 registers */
#define APDS_ENABLE 0x80
#define APDS_ATIME 0x81
#define APDS_CONTROL 0x8F
#define APDS_ID 0x92
#define APDS_CDATAL 0x94
#define APDS_PDATA 0x9C
#define APDS_ENABLE_PON_AEN_PEN 0x07

/* LSM6DSOX registers */
#define LSM_WHO_AM_I 0x0F
#define LSM_WHO_AM_I_VALUE 0x6C
#define LSM_CTRL1_XL 0x10
#define LSM_CTRL3_C 0x12
#define LSM_CTRL10_C 0x19
#define LSM_OUTX_L_A 0x28
#define LSM_STEP_COUNTER 0x4B
#define LSM_TAP_CFG 0x58

enum { SCD_IDLE, SCD_STOPPING, SCD_RUNNING };

static const char *const dev_names[DEV_COUNT] = { "scd40", "sgp40", "apds9960", "lsm6dsox" };

void readings_clear(struct readings *r)
{
    r->co2 = r->temp_c = r->rh = r->voc = NAN;
    r->light = r->proximity = r->motion = r->steps = r->db = NAN;
}

/* ---------- pure helpers ---------- */

uint8_t sensirion_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

double scd40_temp_c(uint16_t raw)
{
    return -45.0 + 175.0 * raw / 65535.0;
}

double scd40_rh(uint16_t raw)
{
    return 100.0 * raw / 65535.0;
}

/* Rounding matches adafruit_sgp40: temperature truncates, RH rounds. */
uint16_t sgp40_temp_ticks(double temp_c)
{
    if (temp_c < -45.0)
        temp_c = -45.0;
    if (temp_c > 130.0)
        temp_c = 130.0;
    return (uint16_t)((temp_c + 45.0) * 65535.0 / 175.0);
}

uint16_t sgp40_rh_ticks(double rh)
{
    if (rh < 0.0)
        rh = 0.0;
    if (rh > 100.0)
        rh = 100.0;
    return (uint16_t)(rh * 65535.0 / 100.0 + 0.5);
}

double lsm6dsox_motion(int16_t x, int16_t y, int16_t z)
{
    const double k = 0.061 * 0.00980665; /* mg/LSB at +-2 g, then mg -> m/s^2 */
    double ax = x * k, ay = y * k, az = z * k;

    return fabs(sqrt(ax * ax + ay * ay + az * az) - 9.81);
}

/* ---------- bus helpers ---------- */

static void sleep_ms(long ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };

    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

static int sens_cmd(int fd, uint8_t addr, uint16_t cmd)
{
    uint8_t b[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };

    return i2c_write(fd, addr, b, sizeof(b));
}

/* Read n (<= 3) CRC-protected words. */
static int sens_read_words(int fd, uint8_t addr, uint16_t *words, size_t n)
{
    uint8_t buf[9];

    if (n == 0 || n > 3) {
        errno = EINVAL;
        return -1;
    }
    if (i2c_read(fd, addr, buf, n * 3) != 0)
        return -1;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *w = &buf[i * 3];
        if (sensirion_crc8(w, 2) != w[2]) {
            errno = EBADMSG;
            return -1;
        }
        words[i] = (uint16_t)((w[0] << 8) | w[1]);
    }
    return 0;
}

/* Sensirion read command: send, wait the 1 ms execution time, read. */
static int sens_query(int fd, uint8_t addr, uint16_t cmd, uint16_t *words, size_t n)
{
    if (sens_cmd(fd, addr, cmd) != 0)
        return -1;
    sleep_ms(1);
    return sens_read_words(fd, addr, words, n);
}

static int reg_write(int fd, uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };

    return i2c_write(fd, addr, b, sizeof(b));
}

static int reg_read(int fd, uint8_t addr, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_write_read(fd, addr, &reg, 1, buf, n);
}

static int reg_set_bits(int fd, uint8_t addr, uint8_t reg, uint8_t bits)
{
    uint8_t v;

    if (reg_read(fd, addr, reg, &v, 1) != 0)
        return -1;
    return reg_write(fd, addr, reg, (uint8_t)(v | bits));
}

/* ---------- device state ---------- */

static void dev_fail(struct sensors *s, enum dev_id id, const char *what, time_t now)
{
    struct dev_state *d = &s->dev[id];
    int err = errno;

    if (!d->logged) {
        fprintf(stderr, "enkakumond: %s: %s failed: %s (retrying every %d s)\n",
                dev_names[id], what, strerror(err), s->conf->retry_s);
        d->logged = true;
    }
    d->up = false;
    d->next_retry = now + s->conf->retry_s;
}

static void dev_ok(struct sensors *s, enum dev_id id)
{
    struct dev_state *d = &s->dev[id];

    if (d->logged) {
        fprintf(stderr, "enkakumond: %s: recovered\n", dev_names[id]);
        d->logged = false;
    }
    d->up = true;
}

/* Returns true if the device is up, or was just brought up. */
static bool dev_ready(struct sensors *s, enum dev_id id, time_t now,
                      int (*init)(struct sensors *), const char *what)
{
    struct dev_state *d = &s->dev[id];

    if (d->up)
        return true;
    if (now < d->next_retry)
        return false;
    if (init(s) != 0) {
        dev_fail(s, id, what, now);
        return false;
    }
    dev_ok(s, id);
    return true;
}

/* ---------- SCD40 ---------- */

static void scd40_forget(struct sensors *s)
{
    s->co2 = s->temp_c = s->rh = NAN;
    s->scd_phase = SCD_IDLE;
}

/* Init is split over two cycles so the 500 ms stop delay never blocks a cycle:
 * IDLE --stop--> STOPPING --(next cycle) start--> RUNNING. */
static void scd40_cycle(struct sensors *s, time_t now)
{
    struct dev_state *d = &s->dev[DEV_SCD40];
    uint16_t w[3];

    if (s->scd_phase == SCD_STOPPING) {
        if (sens_cmd(s->fd, SCD40_ADDR, SCD40_START_PERIODIC) != 0) {
            dev_fail(s, DEV_SCD40, "start", now);
            scd40_forget(s);
            return;
        }
        s->scd_phase = SCD_RUNNING;
        dev_ok(s, DEV_SCD40);
        return;
    }
    if (!d->up) {
        if (now < d->next_retry)
            return;
        if (sens_cmd(s->fd, SCD40_ADDR, SCD40_STOP_PERIODIC) != 0) {
            dev_fail(s, DEV_SCD40, "init", now);
            return;
        }
        s->scd_phase = SCD_STOPPING;
        return;
    }

    if (sens_query(s->fd, SCD40_ADDR, SCD40_DATA_READY, w, 1) != 0) {
        dev_fail(s, DEV_SCD40, "data ready", now);
        scd40_forget(s);
        return;
    }
    if ((w[0] & 0x07FF) == 0)
        return; /* keep last values; new data every 5 s */
    if (sens_query(s->fd, SCD40_ADDR, SCD40_READ_MEASUREMENT, w, 3) != 0) {
        dev_fail(s, DEV_SCD40, "read", now);
        scd40_forget(s);
        return;
    }
    s->co2 = w[0];
    s->temp_c = scd40_temp_c(w[1]) - s->conf->temp_offset_c;
    s->rh = scd40_rh(w[2]);
}

/* ---------- SGP40 ---------- */

static int sgp40_init(struct sensors *s)
{
    uint16_t serial[3];

    return sens_query(s->fd, SGP40_ADDR, SGP40_GET_SERIAL, serial, 3);
}

static void sgp40_cycle(struct sensors *s, struct readings *r, time_t now)
{
    if (!dev_ready(s, DEV_SGP40, now, sgp40_init, "init"))
        return;

    double t = isnan(s->temp_c) ? s->conf->default_temp_c : s->temp_c;
    double rh = isnan(s->rh) ? s->conf->default_rh : s->rh;
    uint16_t rh_ticks = sgp40_rh_ticks(rh);
    uint16_t t_ticks = sgp40_temp_ticks(t);
    uint8_t cmd[8] = {
        SGP40_MEASURE_RAW >> 8, SGP40_MEASURE_RAW & 0xFF,
        (uint8_t)(rh_ticks >> 8), (uint8_t)(rh_ticks & 0xFF), 0,
        (uint8_t)(t_ticks >> 8), (uint8_t)(t_ticks & 0xFF), 0,
    };
    cmd[4] = sensirion_crc8(&cmd[2], 2);
    cmd[7] = sensirion_crc8(&cmd[5], 2);

    uint16_t raw;
    if (i2c_write(s->fd, SGP40_ADDR, cmd, sizeof(cmd)) != 0) {
        dev_fail(s, DEV_SGP40, "measure", now);
        return;
    }
    sleep_ms(30);
    if (sens_read_words(s->fd, SGP40_ADDR, &raw, 1) != 0) {
        dev_fail(s, DEV_SGP40, "read", now);
        return;
    }

    int32_t index;
    GasIndexAlgorithm_process(&s->gas, (int32_t)raw, &index);
    r->voc = index;
}

/* ---------- APDS9960 ---------- */

static int apds_init(struct sensors *s)
{
    uint8_t id;

    if (reg_read(s->fd, APDS9960_ADDR, APDS_ID, &id, 1) != 0)
        return -1;
    if (id != 0xAB && id != 0xA8) {
        errno = ENODEV;
        return -1;
    }
    /* Same integration time (712 ms) and gain (4x) as the Adafruit defaults. */
    if (reg_write(s->fd, APDS9960_ADDR, APDS_ENABLE, 0x00) != 0 ||
        reg_write(s->fd, APDS9960_ADDR, APDS_ATIME, 0x00) != 0 ||
        reg_write(s->fd, APDS9960_ADDR, APDS_CONTROL, 0x01) != 0 ||
        reg_write(s->fd, APDS9960_ADDR, APDS_ENABLE, APDS_ENABLE_PON_AEN_PEN) != 0)
        return -1;
    sleep_ms(10); /* power-on delay */
    return 0;
}

static void apds_cycle(struct sensors *s, struct readings *r, time_t now)
{
    uint8_t prox, c[2];

    if (!dev_ready(s, DEV_APDS9960, now, apds_init, "init"))
        return;
    if (reg_read(s->fd, APDS9960_ADDR, APDS_PDATA, &prox, 1) != 0 ||
        reg_read(s->fd, APDS9960_ADDR, APDS_CDATAL, c, 2) != 0) {
        dev_fail(s, DEV_APDS9960, "read", now);
        return;
    }
    r->proximity = prox;
    r->light = (uint16_t)(c[0] | (c[1] << 8));
}

/* ---------- LSM6DSOX ---------- */

static int lsm_init(struct sensors *s)
{
    uint8_t v;

    if (reg_read(s->fd, LSM6DSOX_ADDR, LSM_WHO_AM_I, &v, 1) != 0)
        return -1;
    if (v != LSM_WHO_AM_I_VALUE) {
        errno = ENODEV;
        return -1;
    }
    if (reg_write(s->fd, LSM6DSOX_ADDR, LSM_CTRL3_C, 0x01) != 0) /* SW_RESET */
        return -1;
    for (int i = 0;; i++) {
        sleep_ms(1);
        if (reg_read(s->fd, LSM6DSOX_ADDR, LSM_CTRL3_C, &v, 1) != 0)
            return -1;
        if ((v & 0x01) == 0)
            break;
        if (i == 10) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
    if (reg_set_bits(s->fd, LSM6DSOX_ADDR, LSM_CTRL3_C, 0x40) != 0 ||   /* BDU */
        reg_write(s->fd, LSM6DSOX_ADDR, LSM_CTRL1_XL, 0x40) != 0)       /* 104 Hz, +-2 g */
        return -1;
    /* Pedometer: mirrors adafruit_lsm6ds pedometer_enable exactly, as the code
     * plan specifies (TAP_CFG bit 6; CTRL10_C FUNC_EN bit 2 and reset bit 1). */
    if (reg_set_bits(s->fd, LSM6DSOX_ADDR, LSM_TAP_CFG, 0x40) != 0 ||
        reg_set_bits(s->fd, LSM6DSOX_ADDR, LSM_CTRL10_C, 0x06) != 0)
        return -1;
    return 0;
}

static void lsm_cycle(struct sensors *s, struct readings *r, time_t now)
{
    uint8_t a[6], st[2];

    if (!dev_ready(s, DEV_LSM6DSOX, now, lsm_init, "init"))
        return;
    if (reg_read(s->fd, LSM6DSOX_ADDR, LSM_OUTX_L_A, a, sizeof(a)) != 0 ||
        reg_read(s->fd, LSM6DSOX_ADDR, LSM_STEP_COUNTER, st, sizeof(st)) != 0) {
        dev_fail(s, DEV_LSM6DSOX, "read", now);
        return;
    }
    int16_t x = (int16_t)(a[0] | (a[1] << 8));
    int16_t y = (int16_t)(a[2] | (a[3] << 8));
    int16_t z = (int16_t)(a[4] | (a[5] << 8));
    r->motion = lsm6dsox_motion(x, y, z);
    r->steps = (int16_t)(st[0] | (st[1] << 8)); /* "<h" like pedometer_steps */
}

/* ---------- public ---------- */

void sensors_init(struct sensors *s, const struct conf *c)
{
    memset(s, 0, sizeof(*s));
    s->conf = c;
    s->fd = -1;
    scd40_forget(s);
    GasIndexAlgorithm_init(&s->gas, GasIndexAlgorithm_ALGORITHM_TYPE_VOC);
}

void sensors_read(struct sensors *s, struct readings *r, time_t now)
{
    if (s->fd < 0) {
        if (now < s->bus_next_retry)
            return;
        s->fd = i2c_open(s->conf->i2c_dev);
        if (s->fd < 0) {
            if (!s->bus_logged) {
                fprintf(stderr, "enkakumond: i2c: open %s failed: %s (retrying every %d s)\n",
                        s->conf->i2c_dev, strerror(errno), s->conf->retry_s);
                s->bus_logged = true;
            }
            s->bus_next_retry = now + s->conf->retry_s;
            return;
        }
        if (s->bus_logged) {
            fprintf(stderr, "enkakumond: i2c: %s recovered\n", s->conf->i2c_dev);
            s->bus_logged = false;
        }
        for (int i = 0; i < DEV_COUNT; i++)
            s->dev[i].next_retry = now;
    }

    scd40_cycle(s, now);
    r->co2 = s->co2;
    r->temp_c = s->temp_c;
    r->rh = s->rh;
    sgp40_cycle(s, r, now);
    apds_cycle(s, r, now);
    lsm_cycle(s, r, now);
}

void sensors_close(struct sensors *s)
{
    if (s->fd < 0)
        return;
    if (sens_cmd(s->fd, SCD40_ADDR, SCD40_STOP_PERIODIC) == 0)
        sleep_ms(500);
    else if (s->scd_phase != SCD_IDLE)
        fprintf(stderr, "enkakumond: scd40: stop failed: %s\n", strerror(errno));
    if (close(s->fd) != 0)
        fprintf(stderr, "enkakumond: i2c: close failed: %s\n", strerror(errno));
    s->fd = -1;
}
