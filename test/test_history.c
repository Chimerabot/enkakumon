/*
 * test_history.c — unit tests for the 48-hour minute history (src/history.c).
 * Run from the repo root by make test, make asan and make memcheck.
 * "history: ..." lines printed during the run are expected.
 */
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "history.h"

static int checks, failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        checks++;                                                                   \
        if (!(cond)) {                                                              \
            failures++;                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

static struct history h; /* large: keep it off the stack */
static char line[8192];

#define T0 ((time_t)1700000000 / 60 * 60) /* a minute boundary */

static struct readings sample(double co2, double steps)
{
    struct readings r;
    readings_clear(&r);
    r.co2 = co2;
    r.temp_c = 22.5;
    r.steps = steps;
    return r;
}

static void test_encoding(void)
{
    const uint8_t v[] = "123456789";
    CHECK(hist_crc32(v, 9) == 0xCBF43926u);

    struct hist_rec a = { .valid = true, .seq = 70000, .t = T0, .steps = 42,
                          .health = 77, .state = 2, .samples = 60 };
    for (int i = 0; i < HV_COUNT; i++)
        a.v[i] = (float)(i * 10.5);
    a.v[HV_DB] = NAN;
    uint8_t b[HIST_REC_SIZE];
    struct hist_rec c;
    hist_encode(&a, b);
    CHECK(memcmp(b, "EKH1", 4) == 0);
    CHECK(hist_decode(b, &c));
    CHECK(c.seq == 70000 && c.t == T0 && c.steps == 42 && c.health == 77 && c.state == 2 &&
          c.samples == 60);
    CHECK(c.v[HV_TEMP] == a.v[HV_TEMP] && isnan(c.v[HV_DB]));
    b[20] ^= 1; /* a torn or corrupt record fails its CRC */
    CHECK(!hist_decode(b, &c));
}

static void test_store(const char *path)
{
    /* minute averaging and steps; nothing is stored until the minute ends */
    history_open(&h, path);
    CHECK(h.fd >= 0 && h.next_seq == 0);
    struct readings r = sample(400, 100);
    history_add(&h, &r, 90, 0, T0);
    r = sample(600, 110);
    history_add(&h, &r, 90, 0, T0 + 59);
    CHECK(h.next_seq == 0);
    r = sample(NAN, 3); /* step counter reset: no negative steps */
    history_add(&h, &r, 89, 0, T0 + 60);
    CHECK(h.next_seq == 1);
    CHECK(h.rec[0].t == T0 && h.rec[0].v[HV_CO2] == 500 && h.rec[0].steps == 10 &&
          h.rec[0].samples == 2 && isnan(h.rec[0].v[HV_DB]));
    r = sample(800, 8);
    history_add(&h, &r, 89, 0, T0 + 61);
    history_close(&h); /* writes the partial minute */
    CHECK(h.next_seq == 2 && h.rec[1].steps == 5 && h.rec[1].v[HV_CO2] == 800);

    /* reopen: records and seq persist */
    history_open(&h, path);
    CHECK(h.next_seq == 2 && h.rec[1].valid && h.rec[1].t == T0 + 60);

    /* a torn record is skipped; appends continue after the highest valid seq */
    history_close(&h);
    int fd = open(path, O_WRONLY);
    CHECK(fd >= 0 && pwrite(fd, "junk", 4, HIST_REC_SIZE + 20) == 4 && close(fd) == 0);
    fprintf(stderr, "--- expected history messages follow ---\n");
    history_open(&h, path);
    CHECK(h.rec[0].valid && !h.rec[1].valid && h.next_seq == 1);
    history_close(&h);

    /* a file of the wrong size starts over */
    fd = open(path, O_WRONLY | O_TRUNC);
    CHECK(fd >= 0 && write(fd, "x", 1) == 1 && close(fd) == 0);
    history_open(&h, path);
    CHECK(h.next_seq == 0 && !h.rec[0].valid);
    CHECK(lseek(h.fd, 0, SEEK_END) == (off_t)HIST_SLOTS * HIST_REC_SIZE);
    history_close(&h);

    /* unusable storage: history still works in RAM */
    history_open(&h, "/nonexistent/dir/history.bin");
    CHECK(h.fd < 0);
    r = sample(400, NAN);
    history_add(&h, &r, 50, 1, T0);
    history_add(&h, &r, 50, 1, T0 + 60);
    CHECK(h.next_seq == 1 && h.rec[0].valid);
    fprintf(stderr, "--- end of expected history messages ---\n");
    history_close(&h);
}

static void test_query(const char *path)
{
    unlink(path);
    history_open(&h, path);
    /* 30 minutes of co2 = minute index, then the device is off for 10 minutes,
     * then 5 more minutes */
    for (int m = 0; m < 30; m++) {
        struct readings r = sample(m, NAN);
        history_add(&h, &r, 100, 0, T0 + 60 * m);
    }
    for (int m = 40; m < 46; m++) {
        struct readings r = sample(m, NAN);
        history_add(&h, &r, 100, 0, T0 + 60 * m);
    }
    /* minutes 0-29 and 40-44 are stored; 45 is still being averaged */
    time_t now = T0 + 60 * 45 + 30;

    /* 10 minutes: one bin per minute (35..44); 35-39 are missing */
    int n = history_format(&h, line, sizeof(line), 10, now);
    CHECK(n > 0 && line[n - 1] == '\n');
    CHECK(strstr(line, "{\"hist\":10,\"co2\":[null,null,null,null,null,40,41,42,43,44],") == line);
    CHECK(strstr(line, "\"temp_c\":[null,null,null,null,null,22.5,22.5,22.5,22.5,22.5]") != NULL);
    CHECK(strstr(line, "\"voc\":[null,null,null,null,null,null,null,null,null,null]") != NULL);
    CHECK(strstr(line, "\"skip\":[5]}\n") != NULL); /* minute 40 follows minute 29 */

    /* 60 minutes: 60 bins; the skip is at minute 40 = bin 55 */
    n = history_format(&h, line, sizeof(line), 60, now);
    CHECK(n > 0 && strstr(line, "\"skip\":[55]}") != NULL);

    /* 48 h: HIST_MAX_BINS bins, everything in the last one or two */
    n = history_format(&h, line, sizeof(line), HIST_SLOTS, now);
    CHECK(n > 0 && strstr(line, "{\"hist\":2880,") == line);
    int commas = 0;
    for (const char *p = strstr(line, "\"co2\":["); *p != ']'; p++)
        commas += *p == ',';
    CHECK(commas == HIST_MAX_BINS - 1);

    /* out-of-range requests are clamped; a small buffer fails cleanly */
    CHECK(history_format(&h, line, sizeof(line), 0, now) > 0 && strstr(line, "{\"hist\":1,") == line);
    CHECK(history_format(&h, line, sizeof(line), 99999, now) > 0 &&
          strstr(line, "{\"hist\":2880,") == line);
    CHECK(history_format(&h, line, 64, 60, now) == -1);
    history_close(&h);
    unlink(path);
}

int main(void)
{
    char dir[] = "/tmp/enkakumon-hist-XXXXXX", path[PATH_MAX];

    if (mkdtemp(dir) == NULL) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(path, sizeof(path), "%s/history.bin", dir);

    test_encoding();
    test_store(path);
    test_query(path);

    unlink(path);
    CHECK(rmdir(dir) == 0);
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
