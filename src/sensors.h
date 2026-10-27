/*
 * sensors.h — SCD40, SGP40, APDS9960 and LSM6DSOX over one I2C bus.
 *
 * Each device is independent: a failure is logged once, the device's values
 * become missing (NAN), and its init is retried every RETRY_S. Nothing here
 * is ever fatal.
 *
 * Acceptance tests (test/test_main.c):
 *   CRC-8 vectors 0xBEEF->0x92, 0x6666->0x93, 0x8000->0xA2, 0xFFFF->0xAC;
 *   SCD40 raw->T/RH; SGP40 25 C -> 0x6666 and 50 %RH -> 0x8000 ticks;
 *   motion is ~0 at rest (1 g) and grows with extra acceleration.
 * On the Pi: breathing on the board raises co2, covering the APDS9960 raises
 * proximity, shaking raises motion; with I2C disconnected every value is
 * missing and the daemon keeps running.
 */
#ifndef ENKAKUMON_SENSORS_H
#define ENKAKUMON_SENSORS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "conf.h"
#include "vendor/sensirion_gas_index_algorithm.h"

/* One cycle of readings. Missing values are NAN. */
struct readings {
    double co2;
    double temp_c;
    double rh;
    double voc;
    double light;
    double proximity;
    double motion;
    double steps;
    double db;
};

void readings_clear(struct readings *r);

enum dev_id { DEV_SCD40, DEV_SGP40, DEV_APDS9960, DEV_LSM6DSOX, DEV_COUNT };

struct dev_state {
    bool up;
    bool logged;       /* failure logged and not yet recovered */
    time_t next_retry; /* monotonic seconds */
};

struct sensors {
    const struct conf *conf;
    int fd; /* I2C bus, -1 while unavailable */
    bool bus_logged;
    time_t bus_next_retry;
    struct dev_state dev[DEV_COUNT];
    int scd_phase;
    double co2, temp_c, rh; /* SCD40 values cached between its 5 s updates */
    GasIndexAlgorithmParams gas;
};

void sensors_init(struct sensors *s, const struct conf *c);

/* One cycle: SCD40 -> SGP40 -> APDS9960 -> LSM6DSOX. now is monotonic seconds.
 * Fills every field of r except db. */
void sensors_read(struct sensors *s, struct readings *r, time_t now);

/* Stops the SCD40 (500 ms) and closes the bus. */
void sensors_close(struct sensors *s);

/* Pure helpers, exposed for tests. */
uint8_t sensirion_crc8(const uint8_t *data, size_t len);
double scd40_temp_c(uint16_t raw);
double scd40_rh(uint16_t raw);
uint16_t sgp40_temp_ticks(double temp_c);
uint16_t sgp40_rh_ticks(double rh);
double lsm6dsox_motion(int16_t x, int16_t y, int16_t z);

#endif
