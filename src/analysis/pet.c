#include "pet.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define STATE_MAX 512 /* a saved state is ~50 B; anything bigger is corrupt */

static const char *const mood_names[] = { "happy", "ok", "sick", "sleeping" };

void pet_init(struct pet *p, const struct conf *c)
{
    memset(p, 0, sizeof(*p));
    p->conf = c;
    p->health = c->pet_health_start;
    p->mood = PET_OK; /* pet_update sets the real mood on its first call */
    p->asleep = false;
}

const char *pet_mood_name(enum pet_mood m)
{
    if ((unsigned)m >= sizeof(mood_names) / sizeof(mood_names[0]))
        return "ok";
    return mood_names[m];
}

/* ---------- rules: written by Navin ---------- */

void pet_update(struct pet *p, const struct readings *r, double dt)
{
    /* TODO(Navin): minute rules, sleep and wake, mood. See pet.h. */
    (void)p;
    (void)r;
    (void)dt;
}

void pet_tap(struct pet *p)
{
    /* TODO(Navin): wake the pet. See pet.h. */
    (void)p;
}

const char *pet_worst(const struct pet *p, const struct readings *r)
{
    /* TODO(Navin): worst current reading as text, or NULL. See pet.h. */
    (void)p;
    (void)r;
    return NULL;
}

/* ---------- persistence ---------- */

int pet_expand_path(char *out, size_t size, const char *path)
{
    int n;

    if (strncmp(path, "~/", 2) == 0) {
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            errno = ENOENT;
            return -1;
        }
        n = snprintf(out, size, "%s/%s", home, path + 2);
    } else {
        n = snprintf(out, size, "%s", path);
    }
    if (n < 0 || (size_t)n >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

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
    char full[PATH_MAX], tmp[PATH_MAX + 8], buf[STATE_MAX];
    int fd = -1, rc = -1;
    const char *step = "expand path";

    if (pet_expand_path(full, sizeof(full), path) != 0)
        goto fail;
    snprintf(tmp, sizeof(tmp), "%s.tmp", full);

    int len = snprintf(buf, sizeof(buf), "health=%d\nmood=%s\nasleep=%d\nt=%lld\n",
                       p->health, pet_mood_name(p->mood), p->asleep ? 1 : 0,
                       (long long)time(NULL));
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        errno = EOVERFLOW;
        step = "format";
        goto fail;
    }

    step = "open";
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        goto fail;
    step = "write";
    if (write_all(fd, buf, (size_t)len) != 0)
        goto fail;
    step = "fsync";
    if (fsync(fd) != 0)
        goto fail;
    step = "close";
    int cr = close(fd);
    fd = -1;
    if (cr != 0)
        goto fail;
    step = "rename";
    if (rename(tmp, full) != 0)
        goto fail;
    step = "fsync directory";
    if (fsync_dir(full) != 0)
        goto fail;
    return 0;

fail:
    fprintf(stderr, "enkakumond: pet: save %s failed at %s: %s\n", path, step, strerror(errno));
    if (fd >= 0 && close(fd) != 0)
        fprintf(stderr, "enkakumond: pet: close %s: %s\n", tmp, strerror(errno));
    if (strcmp(step, "expand path") != 0 && strcmp(step, "format") != 0 &&
        unlink(tmp) != 0 && errno != ENOENT)
        fprintf(stderr, "enkakumond: pet: unlink %s: %s\n", tmp, strerror(errno));
    return rc;
}

static int parse_int(const char *s, long lo, long hi, long *out)
{
    char *end;

    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi)
        return -1;
    *out = v;
    return 0;
}

int pet_load(struct pet *p, const char *path)
{
    const struct conf *c = p->conf;
    char full[PATH_MAX], line[STATE_MAX];
    const char *why = NULL;
    long health = -1, asleep = 0;
    int mood = -1;
    FILE *f = NULL;

    if (pet_expand_path(full, sizeof(full), path) != 0) {
        why = strerror(errno);
        goto defaults;
    }
    f = fopen(full, "re");
    if (f == NULL) {
        why = errno == ENOENT ? "no saved state" : strerror(errno);
        goto defaults;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';
        else if (!feof(f)) {
            why = "line too long";
            goto defaults;
        }
        if (len == 0)
            continue;

        char *eq = strchr(line, '=');
        if (eq == NULL) {
            why = "line without '='";
            goto defaults;
        }
        *eq = '\0';
        const char *key = line, *val = eq + 1;

        if (strcmp(key, "health") == 0) {
            if (parse_int(val, 0, 100, &health) != 0) {
                why = "bad health";
                goto defaults;
            }
        } else if (strcmp(key, "asleep") == 0) {
            if (parse_int(val, 0, 1, &asleep) != 0) {
                why = "bad asleep";
                goto defaults;
            }
        } else if (strcmp(key, "mood") == 0) {
            mood = -1;
            for (int i = 0; i < (int)(sizeof(mood_names) / sizeof(mood_names[0])); i++)
                if (strcmp(val, mood_names[i]) == 0)
                    mood = i;
            if (mood < 0) {
                why = "bad mood";
                goto defaults;
            }
        }
        /* other keys (t, future additions) are informational */
    }
    if (ferror(f)) {
        why = "read error";
        goto defaults;
    }
    if (health < 0) {
        why = "no health value";
        goto defaults;
    }

    if (fclose(f) != 0)
        fprintf(stderr, "enkakumond: pet: close %s: %s\n", path, strerror(errno));
    pet_init(p, c);
    p->health = (int)health;
    p->asleep = asleep != 0;
    if (mood >= 0)
        p->mood = (enum pet_mood)mood;
    return 0;

defaults:
    fprintf(stderr, "enkakumond: pet: %s: %s, using defaults\n", path, why);
    if (f != NULL && fclose(f) != 0)
        fprintf(stderr, "enkakumond: pet: close %s: %s\n", path, strerror(errno));
    pet_init(p, c);
    return -1;
}
