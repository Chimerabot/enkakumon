/*
 * pet.h — the enkakumon health system: health, mood, sleep, persistence.
 *
 * Rules (code plan Step 3; thresholds are PET_* keys in enkakumon.conf):
 *   Once per minute of awake time, health changes by the sum of:
 *     co2 minute average  > CO2_WARN / > CO2_BAD     -1 / -2
 *     voc minute average  > VOC_WARN / > VOC_BAD     -1 / -2
 *     db  minute average  > DB_WARN  / > DB_BAD      -1 / -2
 *     temp_c outside TEMP_MIN..TEMP_MAX or rh outside RH_MIN..RH_MAX    -1
 *     >= ACTIVE_STEPS new steps, or average motion > ACTIVE_MOTION      +1
 *     co2 and voc (those with samples) all <= their WARN, at least one  +1
 *   Averages use only non-null samples; a reading with no samples that
 *   minute does not count. A step count that goes backwards (sensor reset)
 *   counts as 0 new steps. Health is clamped to 0..100.
 *   Sleep: proximity > SLEEP_PROXIMITY or light < SLEEP_LIGHT continuously
 *   for SLEEP_S seconds -> asleep; rules pause. A tap or motion >
 *   WAKE_MOTION wakes it, and the minute starts over.
 *   Mood: sleeping while asleep; else health >= HAPPY_MIN happy,
 *   >= OK_MIN ok, otherwise sick.
 *
 * Persistence: pet_save writes key=value lines to <path>.tmp, fsyncs it,
 * renames it over <path> and fsyncs the directory, so after a power cut the
 * file holds the old state or the new one, never a mix. pet_load falls back
 * to the defaults (and logs why) when the file is missing or corrupt.
 *
 * Acceptance tests (test/test_pet.c): each rule and their sum, clamping,
 * skipping nulls, sleep entry and wake, mood boundaries, save/load
 * round-trip, missing and corrupt files, and no .tmp left behind.
 */
#ifndef ENKAKUMON_PET_H
#define ENKAKUMON_PET_H

#include <stdbool.h>
#include <stddef.h>

#include "conf.h"
#include "sensors.h"

enum pet_mood { PET_HAPPY, PET_OK, PET_SICK, PET_SLEEPING };

struct pet {
    const struct conf *conf; /* thresholds (PET_* keys) */
    int health;              /* 0..100 */
    enum pet_mood mood;      /* recomputed by every pet_update */
    bool asleep;

    /* Working state for the rules (minute accumulators, sleep timer).
     * Owned by pet_update: add the fields it needs here. pet_init and
     * pet_load zero them. */
};

/* Defaults: health = PET_HEALTH_START, awake, working state zeroed. */
void pet_init(struct pet *p, const struct conf *c);

/* --- Rules: written by Navin (src/analysis/pet.c) --- */

/* Called once per cycle with that cycle's readings (NAN = missing) and the
 * seconds since the previous call. Applies the minute rules, sleep and wake,
 * and sets p->mood. */
void pet_update(struct pet *p, const struct readings *r, double dt);

/* A tap from the UI: wakes the pet if it is asleep. */
void pet_tap(struct pet *p);

/* The worst current reading as short text ("CO₂ high", "Too noisy"...),
 * or NULL if every reading is within its WARN range. */
const char *pet_worst(const struct pet *p, const struct readings *r);

/* --- Helpers and persistence --- */

const char *pet_mood_name(enum pet_mood m);

/* Expands a leading "~/" using $HOME. Returns 0, or -1 if it does not fit
 * or $HOME is unset. */
int pet_expand_path(char *out, size_t size, const char *path);

/* Returns 0, or -1 after logging (the previous file is left intact). */
int pet_save(const struct pet *p, const char *path);

/* Returns 0 if the file was loaded; otherwise logs why, applies the
 * defaults (pet_init) and returns -1. */
int pet_load(struct pet *p, const char *path);

#endif
