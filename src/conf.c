#include "conf.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_LEN 512

enum kind { K_INT, K_DBL, K_STR };

struct key {
    const char *name;
    enum kind kind;
    size_t off;
};

static const struct key keys[] = {
    { "SAMPLE_HZ", K_INT, offsetof(struct conf, sample_hz) },
    { "RETRY_S", K_INT, offsetof(struct conf, retry_s) },
    { "I2C_DEV", K_STR, offsetof(struct conf, i2c_dev) },
    { "TEMP_OFFSET_C", K_DBL, offsetof(struct conf, temp_offset_c) },
    { "DEFAULT_TEMP_C", K_DBL, offsetof(struct conf, default_temp_c) },
    { "DEFAULT_RH", K_DBL, offsetof(struct conf, default_rh) },
    { "MIC_NAME", K_STR, offsetof(struct conf, mic_name) },
    { "MIC_RATE", K_INT, offsetof(struct conf, mic_rate) },
    { "MIC_PERIOD_FRAMES", K_INT, offsetof(struct conf, mic_period_frames) },
    { "DB_OFFSET", K_DBL, offsetof(struct conf, db_offset) },
    { "SOCKET_PATH", K_STR, offsetof(struct conf, socket_path) },
    { "MAX_CLIENTS", K_INT, offsetof(struct conf, max_clients) },
    { "HEARTBEAT_PATH", K_STR, offsetof(struct conf, heartbeat_path) },
    { "PET_PATH", K_STR, offsetof(struct conf, pet_path) },
    { "PET_SAVE_S", K_INT, offsetof(struct conf, pet_save_s) },
    { "PET_HEALTH_START", K_INT, offsetof(struct conf, pet_health_start) },
    { "PET_CO2_WARN", K_DBL, offsetof(struct conf, pet_co2_warn) },
    { "PET_CO2_BAD", K_DBL, offsetof(struct conf, pet_co2_bad) },
    { "PET_VOC_WARN", K_DBL, offsetof(struct conf, pet_voc_warn) },
    { "PET_VOC_BAD", K_DBL, offsetof(struct conf, pet_voc_bad) },
    { "PET_DB_WARN", K_DBL, offsetof(struct conf, pet_db_warn) },
    { "PET_DB_BAD", K_DBL, offsetof(struct conf, pet_db_bad) },
    { "PET_TEMP_MIN", K_DBL, offsetof(struct conf, pet_temp_min) },
    { "PET_TEMP_MAX", K_DBL, offsetof(struct conf, pet_temp_max) },
    { "PET_RH_MIN", K_DBL, offsetof(struct conf, pet_rh_min) },
    { "PET_RH_MAX", K_DBL, offsetof(struct conf, pet_rh_max) },
    { "PET_ACTIVE_STEPS", K_INT, offsetof(struct conf, pet_active_steps) },
    { "PET_ACTIVE_MOTION", K_DBL, offsetof(struct conf, pet_active_motion) },
    { "PET_SLEEP_PROXIMITY", K_DBL, offsetof(struct conf, pet_sleep_proximity) },
    { "PET_SLEEP_LIGHT", K_DBL, offsetof(struct conf, pet_sleep_light) },
    { "PET_SLEEP_S", K_INT, offsetof(struct conf, pet_sleep_s) },
    { "PET_WAKE_MOTION", K_DBL, offsetof(struct conf, pet_wake_motion) },
    { "PET_HAPPY_MIN", K_INT, offsetof(struct conf, pet_happy_min) },
    { "PET_OK_MIN", K_INT, offsetof(struct conf, pet_ok_min) },
};

void conf_defaults(struct conf *c)
{
    memset(c, 0, sizeof(*c));
    c->sample_hz = 1;
    c->retry_s = 30;
    strcpy(c->i2c_dev, "/dev/i2c-1");
    c->temp_offset_c = 0.0;
    c->default_temp_c = 25.0;
    c->default_rh = 50.0;
    strcpy(c->mic_name, "googlevoicehat");
    c->mic_rate = 48000;
    c->mic_period_frames = 4800;
    c->db_offset = 120.0;
    strcpy(c->socket_path, "/tmp/enkakumon.sock");
    c->max_clients = 4;
    strcpy(c->heartbeat_path, "/dev/shm/enkakumon.heartbeat");

    strcpy(c->pet_path, "~/enkakumon/pet.state");
    c->pet_save_s = 60;
    c->pet_health_start = 80;
    c->pet_co2_warn = 1000;
    c->pet_co2_bad = 1500;
    c->pet_voc_warn = 150;
    c->pet_voc_bad = 250;
    c->pet_db_warn = 70;
    c->pet_db_bad = 85;
    c->pet_temp_min = 18;
    c->pet_temp_max = 26;
    c->pet_rh_min = 30;
    c->pet_rh_max = 60;
    c->pet_active_steps = 20;
    c->pet_active_motion = 0.5;
    c->pet_sleep_proximity = 200;
    c->pet_sleep_light = 10;
    c->pet_sleep_s = 30;
    c->pet_wake_motion = 1.0;
    c->pet_happy_min = 70;
    c->pet_ok_min = 40;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
    return s;
}

static int set_value(struct conf *c, const struct key *k, const char *val)
{
    char *end = NULL;
    void *dst = (char *)c + k->off;

    switch (k->kind) {
    case K_INT: {
        errno = 0;
        long v = strtol(val, &end, 10);
        if (errno != 0 || end == val || *end != '\0' || v < INT_MIN || v > INT_MAX)
            return -1;
        *(int *)dst = (int)v;
        return 0;
    }
    case K_DBL: {
        errno = 0;
        double v = strtod(val, &end);
        if (errno != 0 || end == val || *end != '\0' || !isfinite(v))
            return -1;
        *(double *)dst = v;
        return 0;
    }
    case K_STR:
        if (strlen(val) >= CONF_STR_MAX)
            return -1;
        strcpy((char *)dst, val);
        return 0;
    }
    return -1;
}

static int validate(const struct conf *c, const char *path)
{
    const char *bad = NULL;

    if (c->sample_hz != 1)
        bad = "SAMPLE_HZ must be 1 (the gas index algorithm needs exactly 1 s)";
    else if (c->retry_s < 1)
        bad = "RETRY_S must be >= 1";
    else if (c->max_clients < 1 || c->max_clients > 64)
        bad = "MAX_CLIENTS must be 1..64";
    else if (c->mic_rate < 8000 || c->mic_rate > 192000)
        bad = "MIC_RATE must be 8000..192000";
    else if (c->mic_period_frames < 1 || c->mic_period_frames > c->mic_rate)
        bad = "MIC_PERIOD_FRAMES must be 1..MIC_RATE";
    else if (c->default_rh < 0.0 || c->default_rh > 100.0)
        bad = "DEFAULT_RH must be 0..100";
    else if (c->i2c_dev[0] == '\0' || c->socket_path[0] == '\0' || c->heartbeat_path[0] == '\0' ||
             c->pet_path[0] == '\0')
        bad = "I2C_DEV, SOCKET_PATH, HEARTBEAT_PATH and PET_PATH must not be empty";
    else if (c->pet_save_s < 1 || c->pet_sleep_s < 1 || c->pet_active_steps < 1)
        bad = "PET_SAVE_S, PET_SLEEP_S and PET_ACTIVE_STEPS must be >= 1";
    else if (c->pet_health_start < 0 || c->pet_health_start > 100)
        bad = "PET_HEALTH_START must be 0..100";
    else if (!(c->pet_co2_warn < c->pet_co2_bad && c->pet_voc_warn < c->pet_voc_bad &&
               c->pet_db_warn < c->pet_db_bad))
        bad = "each PET_*_WARN must be below its PET_*_BAD";
    else if (!(c->pet_temp_min < c->pet_temp_max && c->pet_rh_min < c->pet_rh_max))
        bad = "PET_TEMP_MIN/PET_RH_MIN must be below their _MAX";
    else if (!(0 <= c->pet_ok_min && c->pet_ok_min < c->pet_happy_min && c->pet_happy_min <= 100))
        bad = "need 0 <= PET_OK_MIN < PET_HAPPY_MIN <= 100";

    if (bad != NULL) {
        fprintf(stderr, "enkakumond: conf: %s: %s\n", path, bad);
        return -1;
    }
    return 0;
}

int conf_load(struct conf *c, const char *path)
{
    int rc = -1;
    int lineno = 0;
    char line[LINE_MAX_LEN];

    FILE *f = fopen(path, "re");
    if (f == NULL) {
        fprintf(stderr, "enkakumond: conf: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        lineno++;
        size_t len = strlen(line);
        if (len == sizeof(line) - 1 && line[len - 1] != '\n' && !feof(f)) {
            fprintf(stderr, "enkakumond: conf: %s:%d: line too long\n", path, lineno);
            goto cleanup;
        }

        char *hash = strchr(line, '#');
        if (hash != NULL)
            *hash = '\0';
        char *s = trim(line);
        if (*s == '\0')
            continue;

        char *eq = strchr(s, '=');
        if (eq == NULL) {
            fprintf(stderr, "enkakumond: conf: %s:%d: expected KEY = value\n", path, lineno);
            goto cleanup;
        }
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);
        if (*key == '\0' || *val == '\0') {
            fprintf(stderr, "enkakumond: conf: %s:%d: empty key or value\n", path, lineno);
            goto cleanup;
        }

        const struct key *k = NULL;
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            if (strcmp(keys[i].name, key) == 0) {
                k = &keys[i];
                break;
            }
        }
        if (k == NULL)
            continue; /* UI-only or future key */

        if (set_value(c, k, val) != 0) {
            fprintf(stderr, "enkakumond: conf: %s:%d: bad value for %s: '%s'\n",
                    path, lineno, key, val);
            goto cleanup;
        }
    }
    if (ferror(f)) {
        fprintf(stderr, "enkakumond: conf: read error on %s\n", path);
        goto cleanup;
    }

    rc = validate(c, path);

cleanup:
    if (fclose(f) != 0 && rc == 0) {
        fprintf(stderr, "enkakumond: conf: close %s: %s\n", path, strerror(errno));
        rc = -1;
    }
    return rc;
}
