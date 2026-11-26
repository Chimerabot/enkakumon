#include "pet.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const state_names[] = { "idle", "sleeping", "unhealthy", "dead" };
static const char *const task_names[] = { NULL, "red", "green", "blue", "walk", "music" };

static void reset_window(struct pet *p)
{
    p->window_s = 0.0;
    memset(p->sum, 0, sizeof(p->sum));
    memset(p->n, 0, sizeof(p->n));
}

/* No task; the next one starts 10-20 min of awake time from now. */
static void schedule_next(struct pet *p)
{
    p->task = TASK_NONE;
    p->task_s = PET_TASK_GAP_MIN + random() % (PET_TASK_GAP_MAX - PET_TASK_GAP_MIN + 1);
}

void pet_init(struct pet *p, const struct conf *c)
{
    memset(p, 0, sizeof(*p));
    p->conf = c;
    p->health = PET_HEALTH_START;
    p->last_steps = NAN;
    schedule_next(p);
}

void pet_start_task(struct pet *p, enum pet_task t)
{
    p->task = t;
    p->task_s = PET_TASK_S;
    p->progress = 0.0;
}

static bool color_shown(const struct pet *p, const struct readings *r)
{
    double want = r->red, a = r->green, b = r->blue;

    if (p->task == TASK_GREEN)
        want = r->green, a = r->red;
    else if (p->task == TASK_BLUE)
        want = r->blue, b = r->red;
    if (isnan(want) || isnan(a) || isnan(b))
        return false;
    return want > 0 && want >= p->conf->pet_color_ratio * a &&
           want >= p->conf->pet_color_ratio * b;
}

static bool task_done(struct pet *p, const struct readings *r, double dt, double new_steps)
{
    switch (p->task) {
    case TASK_WALK:
        p->progress += new_steps;
        return p->progress >= p->conf->pet_walk_steps;
    case TASK_MUSIC:
        if (!isnan(r->db) && r->db >= p->conf->pet_music_db)
            p->progress += dt;
        return p->progress >= p->conf->pet_music_s;
    default:
        return color_shown(p, r);
    }
}

void pet_update(struct pet *p, const struct readings *r, double dt)
{
    const struct conf *c = p->conf;
    double new_steps = 0.0;

    /* The step counter restarts at 0 when the sensor re-inits: never negative. */
    if (!isnan(r->steps)) {
        if (!isnan(p->last_steps) && r->steps > p->last_steps)
            new_steps = r->steps - p->last_steps;
        p->last_steps = r->steps;
    }
    if (p->health == 0)
        return; /* dead until a tap hatches a new pet */

    /* Dark but not covered means night: a covered pet is smothered, not asleep. */
    bool covered = !isnan(r->proximity) && r->proximity > c->pet_prox_max;
    bool dark = !isnan(r->light) && r->light < c->pet_dark_light && !covered;
    p->dark_s = dark ? p->dark_s + dt : 0.0;
    p->asleep = p->dark_s >= c->pet_sleep_s;
    if (p->asleep) {
        if (p->task != TASK_NONE)
            schedule_next(p); /* cancelled, no penalty */
        return;
    }

    const double v[SIG_COUNT] = { r->co2, r->voc, r->db, r->proximity };
    for (int i = 0; i < SIG_COUNT; i++) {
        if (!isnan(v[i])) {
            p->sum[i] += v[i];
            p->n[i]++;
        }
    }
    p->window_s += dt;
    if (p->window_s >= PET_UPDATE_S) {
        const double max[SIG_COUNT] = { c->pet_co2_max, c->pet_voc_max, c->pet_db_max, c->pet_prox_max };
        bool seen = false, bad = false;
        for (int i = 0; i < SIG_COUNT; i++) {
            if (p->n[i] > 0) {
                seen = true;
                bad = bad || p->sum[i] / p->n[i] > max[i];
            }
        }
        if (seen)
            p->health += bad ? -1 : 1;
        reset_window(p);
    }

    if (p->task == TASK_NONE) {
        p->task_s -= dt;
        if (p->task_s <= 0) {
            long k = random() % 3;
            pet_start_task(p, k == 0 ? (enum pet_task)(TASK_RED + random() % 3)
                              : k == 1 ? TASK_WALK : TASK_MUSIC);
        }
    } else if (task_done(p, r, dt, new_steps)) {
        p->health += PET_TASK_REWARD;
        schedule_next(p);
    } else if ((p->task_s -= dt) <= 0) {
        p->health -= PET_TASK_REWARD;
        schedule_next(p);
    }

    if (p->health > 100)
        p->health = 100;
    if (p->health <= 0) {
        p->health = 0;
        p->task = TASK_NONE;
    }
}

void pet_tap(struct pet *p)
{
    if (p->health == 0)
        pet_init(p, p->conf);
}

enum pet_state pet_state(const struct pet *p)
{
    if (p->health == 0)
        return PET_DEAD;
    if (p->asleep)
        return PET_SLEEPING;
    return p->health <= PET_UNHEALTHY_AT ? PET_UNHEALTHY : PET_IDLE;
}

const char *pet_state_name(enum pet_state s)
{
    return state_names[s];
}

const char *pet_task_name(enum pet_task t)
{
    return task_names[t];
}

/* ---------- persistence ---------- */

/* fsync the directory holding path, so the rename itself is durable. */
static int fsync_dir(const char *path)
{
    char dir[PATH_MAX];
    const char *slash = strrchr(path, '/');

    if (slash == NULL)
        strcpy(dir, ".");
    else if (slash == path)
        strcpy(dir, "/");
    else {
        size_t len = (size_t)(slash - path);
        if (len >= sizeof(dir)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(dir, path, len);
        dir[len] = '\0';
    }

    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int rc = fsync(fd);
    int err = errno;
    if (close(fd) != 0 && rc == 0)
        return -1;
    errno = err;
    return rc;
}

int pet_save(const struct pet *p, const char *path)
{
    char tmp[PATH_MAX], buf[32];
    int fd = -1;
    const char *step = "path";

    int len = snprintf(buf, sizeof(buf), "health=%d\n", p->health);
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        goto fail;
    }

    step = "open";
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        goto fail;
    step = "write";
    ssize_t w = write(fd, buf, (size_t)len);
    if (w != len) {
        if (w >= 0)
            errno = EIO; /* short write */
        goto fail;
    }
    step = "fsync";
    if (fsync(fd) != 0)
        goto fail;
    step = "close";
    int cr = close(fd);
    fd = -1;
    if (cr != 0)
        goto fail;
    step = "rename";
    if (rename(tmp, path) != 0)
        goto fail;
    step = "fsync directory";
    if (fsync_dir(path) != 0)
        goto fail;
    return 0;

fail:
    fprintf(stderr, "enkakumond: pet: save %s failed at %s: %s\n", path, step, strerror(errno));
    if (fd >= 0 && close(fd) != 0)
        fprintf(stderr, "enkakumond: pet: close %s: %s\n", tmp, strerror(errno));
    if (strcmp(step, "path") != 0 && unlink(tmp) != 0 && errno != ENOENT)
        fprintf(stderr, "enkakumond: pet: unlink %s: %s\n", tmp, strerror(errno));
    return -1;
}

int pet_load(struct pet *p, const char *path)
{
    char line[32];
    const char *why = "corrupt";

    FILE *f = fopen(path, "re");
    if (f == NULL) {
        fprintf(stderr, "enkakumond: pet: %s: %s, starting a new pet\n", path,
                errno == ENOENT ? "no saved state" : strerror(errno));
        return -1;
    }
    if (fgets(line, sizeof(line), f) != NULL && strncmp(line, "health=", 7) == 0) {
        char *end;
        errno = 0;
        long v = strtol(line + 7, &end, 10);
        if (errno == 0 && end != line + 7 && (*end == '\n' || *end == '\0') && v >= 0 && v <= 100) {
            p->health = (int)v;
            why = NULL;
        }
    }
    if (ferror(f))
        why = "read error";
    if (fclose(f) != 0)
        fprintf(stderr, "enkakumond: pet: close %s: %s\n", path, strerror(errno));
    if (why != NULL) {
        fprintf(stderr, "enkakumond: pet: %s: %s, starting a new pet\n", path, why);
        return -1;
    }
    return 0;
}
