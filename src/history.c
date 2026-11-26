#include "history.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAGIC 0x31484B45u /* "EKH1" */
#define FILE_SIZE ((off_t)HIST_SLOTS * HIST_REC_SIZE)

/* ---------- record encoding ---------- */

uint32_t hist_crc32(const uint8_t *data, size_t len)
{
    uint32_t c = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++) {
        c ^= data[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void hist_encode(const struct hist_rec *r, uint8_t out[HIST_REC_SIZE])
{
    memset(out, 0, HIST_REC_SIZE);
    put32(out, MAGIC);
    put32(out + 4, r->seq);
    put32(out + 8, (uint32_t)(uint64_t)r->t);
    put32(out + 12, (uint32_t)((uint64_t)r->t >> 32));
    for (int i = 0; i < HV_COUNT; i++) {
        uint32_t bits;
        memcpy(&bits, &r->v[i], sizeof(bits));
        put32(out + 16 + 4 * i, bits);
    }
    put16(out + 48, r->steps);
    out[50] = r->health;
    out[51] = r->state;
    out[52] = r->samples;
    put32(out + 60, hist_crc32(out, 60));
}

bool hist_decode(const uint8_t in[HIST_REC_SIZE], struct hist_rec *r)
{
    if (get32(in) != MAGIC || get32(in + 60) != hist_crc32(in, 60))
        return false;
    r->valid = true;
    r->seq = get32(in + 4);
    r->t = (int64_t)((uint64_t)get32(in + 8) | (uint64_t)get32(in + 12) << 32);
    for (int i = 0; i < HV_COUNT; i++) {
        uint32_t bits = get32(in + 16 + 4 * i);
        memcpy(&r->v[i], &bits, sizeof(bits));
    }
    r->steps = (uint16_t)(in[48] | in[49] << 8);
    r->health = in[50];
    r->state = in[51];
    r->samples = in[52];
    return true;
}

/* ---------- file ---------- */

static void storage_failed(struct history *h, const char *what)
{
    if (!h->logged)
        fprintf(stderr, "enkakumond: history: %s %s: %s (history kept in RAM)\n",
                what, h->path, strerror(errno));
    h->logged = true;
}

void history_open(struct history *h, const char *path)
{
    static uint8_t buf[HIST_SLOTS * HIST_REC_SIZE];
    struct stat st;
    int torn = 0;

    memset(h, 0, sizeof(*h));
    h->fd = -1;
    h->minute = -1;
    h->last_steps = NAN;
    snprintf(h->path, sizeof(h->path), "%s", path);

    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        storage_failed(h, "open");
        return;
    }
    if (fstat(fd, &st) != 0) {
        storage_failed(h, "fstat");
        goto fail;
    }
    if (st.st_size != FILE_SIZE) {
        if (st.st_size != 0)
            fprintf(stderr, "enkakumond: history: %s has the wrong size, starting a new file\n", path);
        if (ftruncate(fd, 0) != 0 || ftruncate(fd, FILE_SIZE) != 0) {
            storage_failed(h, "preallocate");
            goto fail;
        }
        h->fd = fd;
        return;
    }

    memset(buf, 0, sizeof(buf));
    for (size_t got = 0; got < sizeof(buf);) {
        ssize_t n = pread(fd, buf + got, sizeof(buf) - got, (off_t)got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            storage_failed(h, "read");
            goto fail;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    for (int i = 0; i < HIST_SLOTS; i++) {
        const uint8_t *b = buf + (size_t)i * HIST_REC_SIZE;
        struct hist_rec r;
        if (hist_decode(b, &r) && r.seq % HIST_SLOTS == (uint32_t)i) {
            h->rec[i] = r;
            if (r.seq + 1 > h->next_seq)
                h->next_seq = r.seq + 1;
        } else if (b[0] != 0 || get32(b + 60) != 0) {
            torn++;
        }
    }
    if (torn > 0)
        fprintf(stderr, "enkakumond: history: skipped %d torn record(s) (power loss?)\n", torn);
    h->fd = fd;
    return;

fail:
    if (close(fd) != 0)
        fprintf(stderr, "enkakumond: history: close %s: %s\n", path, strerror(errno));
}

/* Writes the minute being averaged, if it has samples, and starts a new one. */
static void flush(struct history *h)
{
    if (h->samples > 0) {
        struct hist_rec r = {
            .valid = true,
            .seq = h->next_seq++,
            .t = h->minute * 60,
            .steps = (uint16_t)(h->steps > 65535 ? 65535 : h->steps),
            .health = (uint8_t)h->health,
            .state = (uint8_t)h->state,
            .samples = (uint8_t)(h->samples > 255 ? 255 : h->samples),
        };
        for (int i = 0; i < HV_COUNT; i++)
            r.v[i] = h->cnt[i] > 0 ? (float)(h->sum[i] / h->cnt[i]) : NAN;
        h->rec[r.seq % HIST_SLOTS] = r;

        if (h->fd >= 0) {
            uint8_t b[HIST_REC_SIZE];
            hist_encode(&r, b);
            off_t off = (off_t)(r.seq % HIST_SLOTS) * HIST_REC_SIZE;
            ssize_t n = pwrite(h->fd, b, sizeof(b), off);
            if (n != (ssize_t)sizeof(b)) {
                if (n >= 0)
                    errno = EIO; /* short write */
                storage_failed(h, "write");
            } else if (fdatasync(h->fd) != 0) {
                storage_failed(h, "sync");
            } else if (h->logged) {
                fprintf(stderr, "enkakumond: history: %s recovered\n", h->path);
                h->logged = false;
            }
        }
    }
    memset(h->sum, 0, sizeof(h->sum));
    memset(h->cnt, 0, sizeof(h->cnt));
    h->samples = 0;
    h->steps = 0.0;
}

void history_add(struct history *h, const struct readings *r, int health, int state, time_t now)
{
    int64_t minute = (int64_t)now / 60;
    if (minute != h->minute) {
        flush(h);
        h->minute = minute;
    }

    const double v[HV_COUNT] = { r->co2, r->temp_c, r->rh, r->voc,
                                 r->light, r->proximity, r->motion, r->db };
    for (int i = 0; i < HV_COUNT; i++) {
        if (!isnan(v[i])) {
            h->sum[i] += v[i];
            h->cnt[i]++;
        }
    }
    /* The step counter restarts at 0 when the sensor re-inits: never negative. */
    if (!isnan(r->steps)) {
        if (!isnan(h->last_steps) && r->steps > h->last_steps)
            h->steps += r->steps - h->last_steps;
        h->last_steps = r->steps;
    }
    h->samples++;
    h->health = health;
    h->state = state;
}

void history_close(struct history *h)
{
    flush(h);
    if (h->fd >= 0 && close(h->fd) != 0)
        fprintf(stderr, "enkakumond: history: close %s: %s\n", h->path, strerror(errno));
    h->fd = -1;
}

/* ---------- query ---------- */

/* Appends to buf at *o; -1 once it does not fit. */
static int app(char *buf, size_t size, size_t *o, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf + *o, size - *o, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size - *o)
        return -1;
    *o += (size_t)n;
    return 0;
}

int history_format(const struct history *h, char *buf, size_t size, int minutes, time_t now)
{
    static const int keys[] = { HV_CO2, HV_TEMP, HV_VOC, HV_DB };
    static const char *const names[] = { "co2", "temp_c", "voc", "db" };
    static const int decimals[] = { 0, 1, 0, 1 };
    enum { NK = sizeof(keys) / sizeof(keys[0]) };
    double sum[NK][HIST_MAX_BINS] = { { 0 } };
    int cnt[NK][HIST_MAX_BINS] = { { 0 } };
    bool skip[HIST_MAX_BINS] = { false };

    if (minutes < 1)
        minutes = 1;
    if (minutes > HIST_SLOTS)
        minutes = HIST_SLOTS;
    int nb = minutes < HIST_MAX_BINS ? minutes : HIST_MAX_BINS;
    int64_t end = (int64_t)now / 60 * 60; /* the current minute is not stored yet */
    int64_t span = (int64_t)minutes * 60, start = end - span;

    /* Oldest to newest by seq: a jump in t between neighbours is a time skip. */
    uint32_t first = h->next_seq > HIST_SLOTS ? h->next_seq - HIST_SLOTS : 0;
    bool have_prev = false;
    int64_t prev_t = 0;
    for (uint32_t s = first; s < h->next_seq; s++) {
        const struct hist_rec *r = &h->rec[s % HIST_SLOTS];
        if (!r->valid || r->seq != s)
            continue;
        if (r->t >= start && r->t < end) {
            int b = (int)((r->t - start) * nb / span);
            for (int k = 0; k < NK; k++) {
                if (!isnan(r->v[keys[k]])) {
                    sum[k][b] += r->v[keys[k]];
                    cnt[k][b]++;
                }
            }
            if (have_prev && (r->t - prev_t > HIST_SKIP_S || r->t < prev_t))
                skip[b] = true;
        }
        prev_t = r->t;
        have_prev = true;
    }

    size_t o = 0;
    if (app(buf, size, &o, "{\"hist\":%d", minutes) != 0)
        return -1;
    for (int k = 0; k < NK; k++) {
        if (app(buf, size, &o, ",\"%s\":[", names[k]) != 0)
            return -1;
        for (int b = 0; b < nb; b++) {
            int rc = cnt[k][b] > 0
                         ? app(buf, size, &o, "%s%.*f", b ? "," : "", decimals[k], sum[k][b] / cnt[k][b])
                         : app(buf, size, &o, "%snull", b ? "," : "");
            if (rc != 0)
                return -1;
        }
        if (app(buf, size, &o, "]") != 0)
            return -1;
    }
    if (app(buf, size, &o, ",\"skip\":[") != 0)
        return -1;
    bool any = false;
    for (int b = 0; b < nb; b++) {
        if (skip[b]) {
            if (app(buf, size, &o, "%s%d", any ? "," : "", b) != 0)
                return -1;
            any = true;
        }
    }
    if (app(buf, size, &o, "]}\n") != 0)
        return -1;
    return (int)o;
}
