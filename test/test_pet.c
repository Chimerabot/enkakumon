/*
 * test_pet.c — unit tests for the health system (src/analysis/pet.c), as
 * specified in docs/enkakumon ux.md.
 * Run from the repo root by make test, make asan and make memcheck.
 * "pet: ... starting a new pet" lines printed during the run are expected.
 */
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "analysis/pet.h"
#include "conf.h"

static int checks, failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        checks++;                                                                   \
        if (!(cond)) {                                                              \
            failures++;                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

static struct conf conf;

/* Readings that are all on the good side, in daylight, nothing covering. */
static struct readings clean(void)
{
    struct readings r;
    readings_clear(&r);
    r.co2 = 600;
    r.temp_c = 22;
    r.voc = 100;
    r.db = 40;
    r.proximity = 5;
    r.light = 500;
    return r;
}

/* A pet with the given health and no surprise due for a long time. */
static struct pet fresh(int health)
{
    struct pet p;
    pet_init(&p, &conf);
    p.health = health;
    p.task_s = 1e9;
    return p;
}

static void run(struct pet *p, const struct readings *r, int seconds)
{
    for (int i = 0; i < seconds; i++)
        pet_update(p, r, 1.0);
}

static void test_defaults(void)
{
    struct pet p;
    pet_init(&p, &conf);
    CHECK(p.health == 100);
    CHECK(pet_state(&p) == PET_IDLE);
    CHECK(p.task == TASK_NONE);
    CHECK(p.task_s >= PET_TASK_GAP_MIN && p.task_s <= PET_TASK_GAP_MAX);
    CHECK(strcmp(pet_state_name(PET_UNHEALTHY), "unhealthy") == 0);
    CHECK(strcmp(pet_state_name(PET_DEAD), "dead") == 0);
    CHECK(pet_task_name(TASK_NONE) == NULL);
    CHECK(strcmp(pet_task_name(TASK_MUSIC), "music") == 0);
}

static void test_health_update(void)
{
    struct readings r = clean();
    struct pet p = fresh(90);

    run(&p, &r, 299);
    CHECK(p.health == 90); /* only once per 5 minutes */
    run(&p, &r, 1);
    CHECK(p.health == 91); /* all good: +1 */

    struct readings bad[] = { clean(), clean(), clean(), clean(), clean() };
    bad[0].co2 = 1400;
    bad[1].temp_c = 31;
    bad[2].voc = 200;
    bad[3].db = 80;
    bad[4].proximity = 250; /* covered */
    for (int i = 0; i < 5; i++) {
        p = fresh(90);
        run(&p, &bad[i], 300);
        CHECK(p.health == 89); /* any one bad: -1 */
    }

    /* averages, not single samples: 150 s at 1500 + 150 s at 400 = 950 */
    p = fresh(90);
    r.co2 = 1500;
    run(&p, &r, 150);
    r.co2 = 400;
    run(&p, &r, 150);
    CHECK(p.health == 91);

    /* missing readings are skipped; with no samples at all, no change */
    readings_clear(&r);
    p = fresh(90);
    run(&p, &r, 300);
    CHECK(p.health == 90);
    r.voc = 200;
    run(&p, &r, 300);
    CHECK(p.health == 89);

    p = fresh(100);
    r = clean();
    run(&p, &r, 300);
    CHECK(p.health == 100); /* clamped */
}

static void test_states_and_sleep(void)
{
    struct pet p = fresh(51);
    CHECK(pet_state(&p) == PET_IDLE);
    p.health = 50;
    CHECK(pet_state(&p) == PET_UNHEALTHY);

    struct readings r = clean();
    r.light = 5;
    p = fresh(80);
    run(&p, &r, 29);
    CHECK(pet_state(&p) == PET_IDLE);
    run(&p, &r, 1);
    CHECK(pet_state(&p) == PET_SLEEPING);
    r.co2 = 2000;
    run(&p, &r, 600);
    CHECK(p.health == 80); /* updates pause while asleep */
    r = clean();
    run(&p, &r, 1);
    CHECK(pet_state(&p) == PET_IDLE);

    /* dark because covered is not sleep */
    r.light = 5;
    r.proximity = 250;
    p = fresh(80);
    run(&p, &r, 60);
    CHECK(pet_state(&p) == PET_IDLE);

    /* a surprise in progress is cancelled by sleep, without a penalty */
    p = fresh(80);
    pet_start_task(&p, TASK_WALK);
    r = clean();
    r.light = 5;
    run(&p, &r, 30);
    CHECK(p.task == TASK_NONE && p.health == 80);
}

static void test_death(void)
{
    struct readings r = clean();
    struct pet p = fresh(1);

    r.db = 90;
    run(&p, &r, 300);
    CHECK(p.health == 0 && pet_state(&p) == PET_DEAD);
    r = clean();
    run(&p, &r, 3000);
    CHECK(p.health == 0 && p.task == TASK_NONE); /* stays dead */
    pet_tap(&p);
    CHECK(p.health == 100 && pet_state(&p) == PET_IDLE);

    p = fresh(70);
    pet_tap(&p);
    CHECK(p.health == 70); /* a tap on a live pet does nothing */

    p = fresh(10);
    pet_start_task(&p, TASK_MUSIC);
    run(&p, &r, 30);
    CHECK(p.health == 0 && p.task == TASK_NONE); /* a failed task can kill */
}

static void test_tasks(void)
{
    struct readings r = clean();
    struct pet p;

    /* a surprise starts when its timer runs out */
    pet_init(&p, &conf);
    p.task_s = 1;
    run(&p, &r, 1);
    CHECK(p.task != TASK_NONE && p.task_s == PET_TASK_S);

    /* walk: 10 new steps; a count that goes backwards adds nothing */
    p = fresh(50);
    r.steps = 100;
    run(&p, &r, 1);
    pet_start_task(&p, TASK_WALK);
    r.steps = 104;
    run(&p, &r, 1);
    r.steps = 3; /* sensor re-init */
    run(&p, &r, 1);
    r.steps = 8;
    run(&p, &r, 1);
    CHECK(p.task == TASK_WALK);
    r.steps = 9;
    run(&p, &r, 1);
    CHECK(p.task == TASK_NONE && p.health == 65);
    CHECK(p.task_s >= PET_TASK_GAP_MIN);

    p = fresh(50);
    pet_start_task(&p, TASK_WALK);
    run(&p, &r, 29);
    CHECK(p.task == TASK_WALK);
    run(&p, &r, 1);
    CHECK(p.task == TASK_NONE && p.health == 35); /* timed out: -15 */

    /* music: 5 s at >= 60 dB, not necessarily in a row */
    r = clean();
    p = fresh(95);
    pet_start_task(&p, TASK_MUSIC);
    r.db = 65;
    run(&p, &r, 3);
    r.db = 40;
    run(&p, &r, 3);
    r.db = 65;
    run(&p, &r, 1);
    CHECK(p.task == TASK_MUSIC);
    run(&p, &r, 1);
    CHECK(p.task == TASK_NONE && p.health == 100); /* +15, clamped */

    p = fresh(50);
    pet_start_task(&p, TASK_MUSIC);
    r.db = 55;
    run(&p, &r, 30);
    CHECK(p.health == 35);

    /* color: the asked channel must be >= 1.5x each of the others */
    r = clean();
    r.red = 300, r.green = 100, r.blue = 100;
    p = fresh(50);
    pet_start_task(&p, TASK_RED);
    run(&p, &r, 1);
    CHECK(p.task == TASK_NONE && p.health == 65);

    r.red = 100, r.green = 300, r.blue = 190;
    p = fresh(50);
    pet_start_task(&p, TASK_GREEN);
    run(&p, &r, 1);
    CHECK(p.health == 65);

    r.red = 100, r.green = 140, r.blue = 200;
    p = fresh(50);
    pet_start_task(&p, TASK_BLUE);
    run(&p, &r, 30); /* 200 < 1.5 x 140 */
    CHECK(p.health == 35);

    readings_clear(&r);
    r.light = 500;
    p = fresh(50);
    pet_start_task(&p, TASK_RED);
    run(&p, &r, 30); /* no color data: not shown */
    CHECK(p.health == 35);
}

static void test_save_load(void)
{
    char dir[] = "/tmp/enkakumon-pet-XXXXXX";
    char path[PATH_MAX], tmp[PATH_MAX + 8];
    struct stat st;

    if (mkdtemp(dir) == NULL) {
        perror("mkdtemp");
        exit(2);
    }
    snprintf(path, sizeof(path), "%s/pet.state", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    struct pet p = fresh(42), q = fresh(100);
    CHECK(pet_save(&p, path) == 0);
    CHECK(stat(tmp, &st) != 0); /* no temp file left behind */
    CHECK(pet_load(&q, path) == 0 && q.health == 42);

    p.health = 0;
    CHECK(pet_save(&p, path) == 0);
    CHECK(pet_load(&q, path) == 0 && pet_state(&q) == PET_DEAD); /* death is saved too */

    fprintf(stderr, "--- expected pet errors follow ---\n");
    const char *bad[] = { "health=abc\n", "health=101\n", "health=-1\n", "junk\n", "", "health=5x\n" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        FILE *f = fopen(path, "w");
        if (f == NULL || fputs(bad[i], f) == EOF || fclose(f) != 0) {
            perror(path);
            exit(2);
        }
        q = fresh(100);
        CHECK(pet_load(&q, path) == -1 && q.health == 100);
    }
    CHECK(unlink(path) == 0);
    CHECK(pet_load(&q, path) == -1 && q.health == 100);

    snprintf(path, sizeof(path), "%s/missing/pet.state", dir);
    CHECK(pet_save(&p, path) == -1);
    fprintf(stderr, "--- end of expected pet errors ---\n");
    CHECK(rmdir(dir) == 0);
}

int main(void)
{
    conf_defaults(&conf);
    srandom(1);

    test_defaults();
    test_health_update();
    test_states_and_sleep();
    test_death();
    test_tasks();
    test_save_load();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
