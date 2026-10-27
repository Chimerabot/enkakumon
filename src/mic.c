#include "mic.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* snd_pcm_wait timeout: bounds how long shutdown waits on a stalled device. */
#define MIC_WAIT_MS 250
#define MIC_STALE_S 2.0

/* ---------- pure helpers ---------- */

double mic_mean_square(const int32_t *x, size_t n)
{
    if (n == 0)
        return 0.0;

    double mean = 0.0;
    for (size_t i = 0; i < n; i++)
        mean += x[i];
    mean /= (double)n;

    double acc = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = x[i] - mean;
        acc += d * d;
    }
    return acc / (double)n;
}

double mic_db_from_mean_square(double mean_square, double db_offset)
{
    if (!(mean_square > 0.0)) /* also rejects NAN */
        return NAN;
    return 20.0 * log10(sqrt(mean_square) / 2147483648.0) + db_offset;
}

/* ---------- capture thread ---------- */

/* Recover from an xrun or suspend and restart capture. */
static int restart(snd_pcm_t *pcm, int err)
{
    err = snd_pcm_recover(pcm, err, 1);
    if (err < 0)
        return err;
    err = snd_pcm_start(pcm);
    return (err < 0 && err != -EBADFD) ? err : 0;
}

static void *capture(void *arg)
{
    struct mic *m = arg;
    snd_pcm_uframes_t want = (snd_pcm_uframes_t)m->conf->mic_period_frames;
    int err = 0;

    while (!atomic_load(&m->stop)) {
        int w = snd_pcm_wait(m->pcm, MIC_WAIT_MS);
        if (w == 0)
            continue; /* timeout: re-check stop */
        if (w < 0) {
            err = restart(m->pcm, w);
            if (err < 0)
                break;
            continue;
        }

        snd_pcm_sframes_t n = snd_pcm_readi(m->pcm, m->buf, want);
        if (n < 0) {
            err = restart(m->pcm, (int)n);
            if (err < 0)
                break;
            continue;
        }
        if (n == 0)
            continue;

        double ms = mic_mean_square(m->buf, (size_t)n);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        pthread_mutex_lock(&m->lock);
        m->ms[m->head] = ms;
        m->at[m->head] = now;
        m->head = (m->head + 1) % m->ring_len;
        if (m->count < m->ring_len)
            m->count++;
        pthread_mutex_unlock(&m->lock);
    }

    if (err < 0) {
        m->err = err;
        atomic_store(&m->failed, true);
    }
    return NULL;
}

/* ---------- device ---------- */

static int find_card(const char *needle)
{
    int card = -1;

    while (snd_card_next(&card) == 0 && card >= 0) {
        char *name = NULL;
        if (snd_card_get_name(card, &name) == 0) {
            bool match = strstr(name, needle) != NULL;
            free(name);
            if (match)
                return card;
        }
    }
    return -1;
}

static int mic_open(struct mic *m)
{
    const struct conf *c = m->conf;
    char dev[32];
    int err;

    int card = find_card(c->mic_name);
    if (card < 0)
        return -ENODEV;
    snprintf(dev, sizeof(dev), "plughw:%d", card);

    /* alsa-lib opens its device fds with O_CLOEXEC. */
    err = snd_pcm_open(&m->pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        m->pcm = NULL;
        return err;
    }
    unsigned latency_us = (unsigned)(4ULL * (unsigned)c->mic_period_frames * 1000000ULL /
                                     (unsigned)c->mic_rate);
    err = snd_pcm_set_params(m->pcm, SND_PCM_FORMAT_S32_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                             1, (unsigned)c->mic_rate, 1, latency_us);
    if (err < 0)
        goto fail;
    err = snd_pcm_start(m->pcm);
    if (err < 0)
        goto fail;

    pthread_mutex_lock(&m->lock);
    m->count = m->head = 0;
    pthread_mutex_unlock(&m->lock);
    atomic_store(&m->stop, false);
    atomic_store(&m->failed, false);
    m->err = 0;

    err = pthread_create(&m->thread, NULL, capture, m);
    if (err != 0) {
        err = -err;
        goto fail;
    }
    m->thread_running = true;
    return 0;

fail:
    if (snd_pcm_close(m->pcm) < 0)
        fprintf(stderr, "enkakumond: mic: close after failed open failed\n");
    m->pcm = NULL;
    return err;
}

static void mic_stop(struct mic *m)
{
    if (m->thread_running) {
        atomic_store(&m->stop, true);
        int err = pthread_join(m->thread, NULL);
        if (err != 0)
            fprintf(stderr, "enkakumond: mic: join failed: %s\n", strerror(err));
        m->thread_running = false;
    }
    if (m->pcm != NULL) {
        int err = snd_pcm_close(m->pcm);
        if (err < 0)
            fprintf(stderr, "enkakumond: mic: close failed: %s\n", snd_strerror(err));
        m->pcm = NULL;
    }
}

static void mic_fail(struct mic *m, const char *what, int err, time_t now)
{
    if (!m->logged) {
        if (err == -ENODEV)
            fprintf(stderr, "enkakumond: mic: no ALSA card matching '%s' (retrying every %d s)\n",
                    m->conf->mic_name, m->conf->retry_s);
        else
            fprintf(stderr, "enkakumond: mic: %s failed: %s (retrying every %d s)\n",
                    what, snd_strerror(err), m->conf->retry_s);
        m->logged = true;
    }
    m->next_retry = now + m->conf->retry_s;
}

/* ---------- public ---------- */

int mic_init(struct mic *m, const struct conf *c)
{
    memset(m, 0, sizeof(*m));
    m->conf = c;
    atomic_init(&m->stop, false);
    atomic_init(&m->failed, false);

    size_t per_s = ((size_t)c->mic_rate + (size_t)c->mic_period_frames - 1) /
                   (size_t)c->mic_period_frames;
    m->ring_len = per_s < 1 ? 1 : (per_s > MIC_RING_MAX ? MIC_RING_MAX : per_s);

    m->buf = malloc((size_t)c->mic_period_frames * sizeof(*m->buf));
    if (m->buf == NULL) {
        fprintf(stderr, "enkakumond: mic: out of memory\n");
        return -1;
    }
    int err = pthread_mutex_init(&m->lock, NULL);
    if (err != 0) {
        fprintf(stderr, "enkakumond: mic: mutex init failed: %s\n", strerror(err));
        free(m->buf);
        m->buf = NULL;
        return -1;
    }
    return 0;
}

void mic_poll(struct mic *m, time_t now)
{
    if (m->thread_running) {
        if (!atomic_load(&m->failed))
            return;
        int err = m->err;
        mic_stop(m);
        mic_fail(m, "capture", err, now);
        return;
    }
    if (now < m->next_retry)
        return;

    int err = mic_open(m);
    if (err < 0) {
        mic_fail(m, "open", err, now);
        return;
    }
    if (m->logged) {
        fprintf(stderr, "enkakumond: mic: recovered\n");
        m->logged = false;
    }
}

double mic_db(struct mic *m)
{
    struct timespec now;
    double sum = 0.0;
    size_t n;

    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&m->lock);
    n = m->count;
    if (n > 0) {
        size_t newest = (m->head + m->ring_len - 1) % m->ring_len;
        double age = (double)(now.tv_sec - m->at[newest].tv_sec) +
                     (double)(now.tv_nsec - m->at[newest].tv_nsec) / 1e9;
        if (age > MIC_STALE_S)
            n = 0;
        for (size_t i = 0; i < n; i++)
            sum += m->ms[i];
    }
    pthread_mutex_unlock(&m->lock);

    if (n == 0)
        return NAN;
    return mic_db_from_mean_square(sum / (double)n, m->conf->db_offset);
}

void mic_close(struct mic *m)
{
    mic_stop(m);
    if (m->buf != NULL) {
        int err = pthread_mutex_destroy(&m->lock);
        if (err != 0)
            fprintf(stderr, "enkakumond: mic: mutex destroy failed: %s\n", strerror(err));
        free(m->buf);
        m->buf = NULL;
    }
    snd_config_update_free_global();
}
