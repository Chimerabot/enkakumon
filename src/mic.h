/*
 * mic.h — SPH0645 I2S mic capture on its own pthread.
 *
 * The thread reads MIC_PERIOD_FRAMES at a time with blocking snd_pcm_readi and
 * stores each period's mean-removed mean square in a mutex-guarded ring that
 * covers ~1 s. mic_db() averages that window; it is NAN when no period has
 * arrived in the last 2 s. A failed device is logged once and reopened every
 * RETRY_S.
 *
 * Acceptance tests (test/test_main.c):
 *   full-scale RMS (2^31) gives exactly DB_OFFSET; RMS = 0 gives NAN;
 *   a pure DC offset has zero mean-removed power.
 * On the Pi: clapping raises db.
 */
#ifndef ENKAKUMON_MIC_H
#define ENKAKUMON_MIC_H

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "conf.h"

#define MIC_RING_MAX 64

struct mic {
    const struct conf *conf;
    snd_pcm_t *pcm;
    int32_t *buf; /* one period, allocated once in mic_init */
    pthread_t thread;
    bool thread_running;
    atomic_bool stop;
    atomic_bool failed; /* set by the thread when the device is unusable */
    int err;            /* ALSA error behind failed; written before failed is set */
    bool logged;
    time_t next_retry;

    pthread_mutex_t lock; /* guards the ring below */
    size_t ring_len;      /* periods per ~1 s window */
    size_t count, head;
    double ms[MIC_RING_MAX];
    struct timespec at[MIC_RING_MAX];
};

/* Returns -1 only if memory or the mutex cannot be set up (fatal). Call with
 * SIGTERM/SIGINT already blocked so the capture thread inherits the mask. */
int mic_init(struct mic *m, const struct conf *c);

/* Once per cycle: reap a failed thread, reopen when RETRY_S has passed. */
void mic_poll(struct mic *m, time_t now);

/* dB SPL over the last ~1 s, or NAN. */
double mic_db(struct mic *m);

void mic_close(struct mic *m);

/* Pure helpers, exposed for tests. */
double mic_mean_square(const int32_t *x, size_t n);
double mic_db_from_mean_square(double mean_square, double db_offset);

#endif
