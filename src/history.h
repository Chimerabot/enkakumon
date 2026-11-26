/*
 * history.h — the 48-hour minute history (docs/DESIGN.md §4).
 *
 * Each wall-clock minute, the averages of the valid samples go into one 64 B
 * record in HISTORY_PATH, a preallocated ring of HIST_SLOTS records (184 KB),
 * at slot seq % HIST_SLOTS, followed by fdatasync. Layout (little-endian):
 *   0 magic "EKH1" | 4 seq u32 | 8 t i64 (start of minute) |
 *   16 co2 temp_c rh voc light proximity motion db (8 x f32, NAN = none) |
 *   48 steps u16 | 50 health u8 | 51 state u8 | 52 samples u8 | 53 zero |
 *   60 CRC-32 of bytes 0-59
 * A torn write fails its CRC and is skipped; appends resume after the highest
 * valid seq. The daemon keeps every record in RAM and answers queries from
 * there. Storage failures are logged once and never fatal.
 *
 * Acceptance tests (test/test_history.c): CRC-32 vector; encode/decode and
 * corruption; minute averaging and steps; persistence across reopen, torn
 * record skipped; query bins, missing bins and time skips.
 */
#ifndef ENKAKUMON_HISTORY_H
#define ENKAKUMON_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "conf.h"
#include "sensors.h"

#define HIST_SLOTS 2880 /* 48 h of minutes */
#define HIST_REC_SIZE 64
#define HIST_MAX_BINS 188 /* one per graph pixel column */
#define HIST_SKIP_S 120   /* a longer jump between records is a time skip */

enum { HV_CO2, HV_TEMP, HV_RH, HV_VOC, HV_LIGHT, HV_PROX, HV_MOTION, HV_DB, HV_COUNT };

struct hist_rec {
    bool valid;
    uint32_t seq;
    int64_t t;
    float v[HV_COUNT];
    uint16_t steps;
    uint8_t health, state, samples;
};

struct history {
    int fd; /* -1 if storage is unavailable; records then live in RAM only */
    bool logged;
    char path[CONF_STR_MAX];
    uint32_t next_seq;
    struct hist_rec rec[HIST_SLOTS];
    /* the minute being averaged */
    int64_t minute;
    double sum[HV_COUNT];
    int cnt[HV_COUNT];
    int samples;
    double steps, last_steps;
    int health, state;
};

/* Loads path, creating or resizing it if needed. */
void history_open(struct history *h, const char *path);

/* One cycle's readings at wall-clock time now. Writes the previous minute's
 * record when the minute changes. */
void history_add(struct history *h, const struct readings *r, int health, int state, time_t now);

/* Writes the partial minute and closes the file. */
void history_close(struct history *h);

/* The last `minutes` (1..HIST_SLOTS) before the current minute as one JSON
 * line, in at most HIST_MAX_BINS bins per graphed metric (null = no data):
 *   {"hist":M,"co2":[...],"temp_c":[...],"voc":[...],"db":[...],"skip":[bin,...]}
 * Returns the length, or -1 if it does not fit. */
int history_format(const struct history *h, char *buf, size_t size, int minutes, time_t now);

/* Exposed for tests. */
uint32_t hist_crc32(const uint8_t *data, size_t len);
void hist_encode(const struct hist_rec *r, uint8_t out[HIST_REC_SIZE]);
bool hist_decode(const uint8_t in[HIST_REC_SIZE], struct hist_rec *r);

#endif
