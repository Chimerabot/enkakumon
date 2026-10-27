/*
 * conf.h — enkakumon.conf parser (KEY = value lines, # comments).
 *
 * Acceptance tests (test/test_main.c):
 *   a valid file loads every known key; comments and blank lines are skipped;
 *   a line without '=', an empty value, a non-numeric number or an over-long
 *   line fails with its line number; unknown keys (UI keys) are ignored.
 */
#ifndef ENKAKUMON_CONF_H
#define ENKAKUMON_CONF_H

/* Large enough for any path below; socket paths are further limited by ipc.c. */
#define CONF_STR_MAX 108

struct conf {
    int sample_hz;
    int retry_s;
    char i2c_dev[CONF_STR_MAX];
    double temp_offset_c;
    double default_temp_c;
    double default_rh;
    char mic_name[CONF_STR_MAX];
    int mic_rate;
    int mic_period_frames;
    double db_offset;
    char socket_path[CONF_STR_MAX];
    int max_clients;
    char heartbeat_path[CONF_STR_MAX];
};

void conf_defaults(struct conf *c);

/* Overlay values from path onto c. Returns 0, or -1 after logging the error. */
int conf_load(struct conf *c, const char *path);

#endif
