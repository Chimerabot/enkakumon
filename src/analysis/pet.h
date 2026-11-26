/*
 * pet.h — the enkakumon health system (docs/enkakumon ux.md).
 *
 * Health is 0..100 and starts at 100. Every PET_UPDATE_S of awake time it
 * moves by 1: -1 if the average of any significant reading (co2, voc, db,
 * proximity) is above its PET_*_MAX, else +1 (no change if none had samples).
 * State: dead at 0; sleeping while dark (light < PET_DARK_LIGHT, not covered)
 * for PET_SLEEP_S; unhealthy at <= 50; otherwise idle. Sleep pauses updates
 * and surprises. A dead pet stays dead until a tap hatches a new one.
 *
 * Surprises: every 10-20 min awake, a task runs for PET_TASK_S. Done in
 * time: +15; not: -15. Color: the asked channel is >= PET_COLOR_RATIO times
 * each other channel. Walk: PET_WALK_STEPS new steps. Music: PET_MUSIC_S
 * seconds at >= PET_MUSIC_DB.
 *
 * Persistence: "health=N" in PET_PATH, written to a temp file, fsynced and
 * renamed, so a power cut leaves the old file or the new one, never a mix.
 *
 * Acceptance tests (test/test_pet.c): update rule and averaging, null
 * skipping, clamping, states, sleep and wake, death and hatching, each task's
 * success and timeout, save/load round-trip and corrupt files.
 */
#ifndef ENKAKUMON_PET_H
#define ENKAKUMON_PET_H

#include <stdbool.h>

#include "conf.h"
#include "sensors.h"

#define PET_HEALTH_START 100
#define PET_UNHEALTHY_AT 50  /* health at or below this is unhealthy */
#define PET_UPDATE_S 300.0   /* health moves once per 5 min of awake time */
#define PET_TASK_S 30.0      /* time to complete a surprise */
#define PET_TASK_REWARD 15
#define PET_TASK_GAP_MIN 600 /* seconds between surprises: 10..20 min */
#define PET_TASK_GAP_MAX 1200

/* Same order as the frames in assets/enkakumon-sprite.png. */
enum pet_state { PET_IDLE, PET_SLEEPING, PET_UNHEALTHY, PET_DEAD };
enum pet_task { TASK_NONE, TASK_RED, TASK_GREEN, TASK_BLUE, TASK_WALK, TASK_MUSIC, TASK_COUNT };
enum { SIG_CO2, SIG_VOC, SIG_DB, SIG_PROX, SIG_COUNT }; /* significant readings */

struct pet {
    const struct conf *conf;
    int health;
    bool asleep;
    double dark_s;            /* how long it has been dark */
    double window_s;          /* awake time in the current update window */
    double sum[SIG_COUNT];
    int n[SIG_COUNT];
    enum pet_task task;
    double task_s;            /* time left on the task, or until the next one */
    double progress;          /* steps (walk) or loud seconds (music) */
    double last_steps;        /* NAN until the first step count */
};

void pet_init(struct pet *p, const struct conf *c);

/* One cycle of readings (NAN = missing), dt seconds after the last one. */
void pet_update(struct pet *p, const struct readings *r, double dt);

/* A tap on the pet: hatches a new pet if this one is dead. */
void pet_tap(struct pet *p);

/* Starts a surprise now (pet_update picks them at random). */
void pet_start_task(struct pet *p, enum pet_task t);

enum pet_state pet_state(const struct pet *p);
const char *pet_state_name(enum pet_state s);
const char *pet_task_name(enum pet_task t); /* NULL for TASK_NONE */

/* Returns 0, or -1 after logging (the previous file is left intact). */
int pet_save(const struct pet *p, const char *path);

/* Returns 0 if loaded; otherwise logs why, keeps the defaults, returns -1. */
int pet_load(struct pet *p, const char *path);

#endif
