#include "pet.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
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

static void reset_minute(struct pet *p) 
{
    p->minute_sec = 0.0;
    p->co2_sum = 0.0;
    p->co2_count = 0;
    p->voc_sum = 0.0;
    p->voc_count = 0;
    p->db_sum = 0.0;
    p->db_count = 0;
    p->temp_c_sum = 0.0;
    p->temp_c_count = 0;
    p->rh_sum = 0.0;
    p->rh_count = 0;
    p->motion_sum = 0.0;
    p->motion_count = 0;
    p->step_sum = 0.0;
}

/*Mood based on the Pet*/
static void update_mood(struct pet *p) 
{
    if (p->asleep) {
        p->mood = PET_SLEEPING;
    } else if(p->health >= p->conf->pet_happy_min) {
        p->mood = PET_HAPPY;
    } else if(p->health >= p->conf->pet_ok_min) {
        p->mood = PET_OK;
    } else {
        p->mood = PET_SICK;
    }
}

void pet_update(struct pet *p, const struct readings *r, double dt)
{

    /* 1. Already asleep: check wake condition */
    if(p->asleep) {
        if(!isnan(r->motion) && r->motion > p->conf->pet_wake_motion) {
            p->asleep = false;
            p->sleep_sec = 0.0;
            reset_minute(p);
            update_mood(p);
            return;
        } else {
            p->mood = PET_SLEEPING;
            return;
        }
    }

    /* 2. Awake: track continuous sleep condition */
    bool covered = (!isnan(r->proximity) && r->proximity > p->conf->pet_sleep_proximity);
    bool dark = (!isnan(r->light) && r->light < p->conf->pet_sleep_light);

    if(covered || dark) {
        p->sleep_sec += dt;
        if(p->sleep_sec >= p->conf->pet_sleep_s) {
            p->asleep = true;
            reset_minute(p);
            p->mood = PET_SLEEPING;
            return;
        }
    } else {
        p->sleep_sec = 0.0;
    }

    /* 3. Accumulate samples */
    if(!isnan(r->co2)) {
        p->co2_sum += r->co2;
        p->co2_count++;
    }
    if(!isnan(r->voc)) {
        p->voc_sum += r->voc;
        p->voc_count++;
    }
    if(!isnan(r->db)) {
        p->db_sum += r->db;
        p->db_count++;
    }
    if(!isnan(r->temp_c)) {
        p->temp_c_sum += r->temp_c;
        p->temp_c_count++;
    }
    if(!isnan(r->rh)) {
        p->rh_sum += r->rh;
        p->rh_count++;
    }
    if(!isnan(r->motion)) {
        p->motion_sum += r->motion;
        p->motion_count++;
    }

    if (!isnan(r->steps)) {
        if (p->has_last_steps) {
            double diff = r->steps - p->last_steps;
            if (diff > 0) {
                p->step_sum += diff;
            }
        }
        p->last_steps = r->steps;
        p->has_last_steps = true;
    }

    /* 4. Evaluate minute rules */
    p->minute_sec += dt;

    if(p->minute_sec >= 60.0) {
        bool co2_clean = true;
        bool voc_clean = true;

        if(p->co2_count > 0) {
            double avg = p->co2_sum / p->co2_count;
            if(avg > p->conf->pet_co2_bad) {
                p->health -= 2;
                co2_clean = false;
            } else if (avg > p->conf->pet_co2_warn) {
                p->health -= 1;
                co2_clean = false;
            }
        }

        /* VOC check*/
        if(p->voc_count > 0) {
            double avg = p->voc_sum / p->voc_count;
            if(avg > p->conf->pet_voc_bad) {
                p->health -= 2;
                voc_clean = false;
            } else if (avg > p->conf->pet_voc_warn) {
                p->health -= 1;
                voc_clean = false;
            }
        }

        /*Noise Check*/
        if(p->db_count > 0) {
            double avg = p->db_sum / p->db_count;
            if(avg > p->conf->pet_db_bad) {
                p->health -= 2;
            } else if (avg > p->conf->pet_db_warn) {
                p->health -= 1;
            }
        }

        /*temp/RH Check*/
        bool temp_out = false;
        bool rh_out = false;
        if(p->temp_c_count > 0) {
            double avg = p->temp_c_sum / p->temp_c_count;
            if(avg < p->conf->pet_temp_min || avg > p->conf->pet_temp_max) {
                temp_out = true;
            }
        }
        if(p->rh_count > 0) {
            double avg = p->rh_sum / p->rh_count;
            if(avg < p->conf->pet_rh_min || avg > p->conf->pet_rh_max ) {
                rh_out = true;
            }
        }
        if(temp_out || rh_out) {
            p->health -= 1;
        }
        /* Motion / steps check (+1 if either threshold is met, not both) */
        bool active_motion = (p->motion_count > 0 && (p->motion_sum / p->motion_count) > p->conf->pet_active_motion);
        bool active_steps = (p->step_sum >= p->conf->pet_active_steps);
        if (active_motion || active_steps) {
            p->health += 1;
        }

        /*clean air check (when CO2 and VOC are both in good/clean range)*/
        if((p->co2_count > 0 || p->voc_count > 0) && co2_clean && voc_clean) {
            p->health += 1;   
        }

        /* clamp health to 0..100*/
        if(p->health > 100) {
            p->health = 100;
        } else if (p->health < 0) {
            p->health = 0;
        }
        reset_minute(p);
    }
    update_mood(p);
}

void pet_tap(struct pet *p)
{
    if(p->asleep) {
        p->asleep = false;
        p->sleep_sec = 0.0;
        reset_minute(p);
    }
}

static int level(double v, double warn, double bad) 
{
    if(v > bad) {
        return 2;
    } else if (v > warn) {
        return 1;
    } else {
        return 0;
    }
}
const char *pet_worst(const struct pet *p, const struct readings *r)
{
   int best_pen = 0;
   const char *best_text = NULL;
   int pen;
   
   /*1: CO2*/
   pen = level(r->co2, p->conf->pet_co2_warn, p->conf->pet_co2_bad);
   if(pen > best_pen) {
    best_pen = pen;
    best_text = "CO₂ high";
   }

   /*2: VOC*/
   pen = level(r->voc, p->conf->pet_voc_warn, p->conf->pet_voc_bad);
   if(pen > best_pen) {
    best_pen = pen;
    best_text = "Air quality poor";
   }

   /*3: Noise*/
   pen = level(r->db, p->conf->pet_db_warn, p->conf->pet_db_bad);
   if(pen > best_pen) {
    best_pen = pen;
    best_text = "Too noisy";
   }

   /*4: Temperature*/
   if(r->temp_c < p->conf->pet_temp_min) {
    if(1 > best_pen) {
        best_pen = 1;
        best_text = "Too cold";
    }
   } else if(r->temp_c > p->conf->pet_temp_max) {
    if (1 > best_pen) {
        best_pen = 1;
        best_text = "Too hot";
    }
   }

   /*5. Relative Humidity*/
   if(r->rh < p->conf->pet_rh_min) {
    if(1> best_pen) {
        best_pen = 1;
        best_text = "Too dry";
    }
   } else if(r->rh > p->conf->pet_rh_max) {
    if(1 > best_pen) {
        best_pen = 1;
        best_text = "Too humid";
    }
   }
   return best_text;
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
