/*
 * test_pet.c — unit tests for the health system (src/analysis/pet.c).
 * Run from the repo root by make test, make asan and make memcheck.
 *
 * Covers: defaults, path expansion, save/load round-trip, missing and
 * corrupt state files, and that a save leaves no .tmp behind; then the
 * rules: each rule alone and their sum, minute averaging, skipping nulls,
 * clamping, mood boundaries, sleep entry and wake, and pet_worst.
 * "pet: ... using defaults" lines printed during the run are expected.
 */
#include <errno.h>
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
static char dir[] = "/tmp/enkakumon-pet-XXXXXX";

static const char *path_in_dir(const char *name)
{
    static char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s/%s", dir, name);
    return buf;
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f == NULL || fputs(text, f) == EOF || fclose(f) != 0) {
        perror(path);
        exit(2);
    }
}

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* ---------- defaults and helpers ---------- */

static void test_defaults(void)
{
    struct pet p;

    pet_init(&p, &conf);
    CHECK(p.health == 80);
    CHECK(!p.asleep);
    CHECK(p.conf == &conf);

    CHECK(strcmp(pet_mood_name(PET_HAPPY), "happy") == 0);
    CHECK(strcmp(pet_mood_name(PET_OK), "ok") == 0);
    CHECK(strcmp(pet_mood_name(PET_SICK), "sick") == 0);
    CHECK(strcmp(pet_mood_name(PET_SLEEPING), "sleeping") == 0);
}

static void test_expand_path(void)
{
    char out[64];
    const char *old = getenv("HOME");
    char saved[PATH_MAX] = "";
    if (old != NULL)
        snprintf(saved, sizeof(saved), "%s", old);

    CHECK(setenv("HOME", "/home/x", 1) == 0);
    CHECK(pet_expand_path(out, sizeof(out), "~/enkakumon/pet.state") == 0);
    CHECK(strcmp(out, "/home/x/enkakumon/pet.state") == 0);
    CHECK(pet_expand_path(out, sizeof(out), "/abs/pet.state") == 0);
    CHECK(strcmp(out, "/abs/pet.state") == 0);
    CHECK(pet_expand_path(out, sizeof(out), "rel/pet.state") == 0);
    CHECK(strcmp(out, "rel/pet.state") == 0);
    CHECK(pet_expand_path(out, 8, "/a/very/long/path") == -1);

    CHECK(unsetenv("HOME") == 0);
    CHECK(pet_expand_path(out, sizeof(out), "~/pet.state") == -1);

    if (saved[0] != '\0')
        CHECK(setenv("HOME", saved, 1) == 0);
}

/* ---------- save / load ---------- */

static void test_round_trip(void)
{
    struct pet a, b;
    const char *path = path_in_dir("pet.state");
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    pet_init(&a, &conf);
    a.health = 37;
    a.mood = PET_SICK;
    a.asleep = true;
    CHECK(pet_save(&a, path) == 0);
    CHECK(exists(path));
    CHECK(!exists(tmp));

    pet_init(&b, &conf);
    CHECK(pet_load(&b, path) == 0);
    CHECK(b.health == 37);
    CHECK(b.mood == PET_SICK);
    CHECK(b.asleep);
    CHECK(b.conf == &conf);

    /* a second save replaces the first */
    a.health = 99;
    a.mood = PET_HAPPY;
    a.asleep = false;
    CHECK(pet_save(&a, path) == 0);
    CHECK(pet_load(&b, path) == 0);
    CHECK(b.health == 99 && b.mood == PET_HAPPY && !b.asleep);
    CHECK(!exists(tmp));

    /* boundaries survive */
    a.health = 0;
    CHECK(pet_save(&a, path) == 0);
    CHECK(pet_load(&b, path) == 0 && b.health == 0);
    a.health = 100;
    CHECK(pet_save(&a, path) == 0);
    CHECK(pet_load(&b, path) == 0 && b.health == 100);

    CHECK(unlink(path) == 0);
}

static void expect_defaults(const char *contents)
{
    struct pet p;
    const char *path = path_in_dir("bad.state");

    write_file(path, contents);
    pet_init(&p, &conf);
    p.health = 5;
    p.asleep = true;
    CHECK(pet_load(&p, path) == -1);
    CHECK(p.health == 80);
    CHECK(!p.asleep);
    CHECK(unlink(path) == 0);
}

static void test_bad_files(void)
{
    struct pet p;

    fprintf(stderr, "--- expected pet errors follow ---\n");

    /* missing file */
    pet_init(&p, &conf);
    p.health = 5;
    CHECK(pet_load(&p, path_in_dir("missing.state")) == -1);
    CHECK(p.health == 80);

    expect_defaults("");
    expect_defaults("garbage\n");
    expect_defaults("health=abc\n");
    expect_defaults("health=101\n");
    expect_defaults("health=-1\n");
    expect_defaults("health=50x\n");
    expect_defaults("mood=happy\nasleep=0\n");          /* no health */
    expect_defaults("health=50\nmood=angry\n");
    expect_defaults("health=50\nasleep=2\n");

    char longline[700];
    memset(longline, 'h', sizeof(longline) - 2);
    longline[sizeof(longline) - 2] = '\n';
    longline[sizeof(longline) - 1] = '\0';
    expect_defaults(longline);

    /* save into a directory that does not exist fails cleanly */
    pet_init(&p, &conf);
    CHECK(pet_save(&p, path_in_dir("no/such/dir/pet.state")) == -1);
    fprintf(stderr, "--- end of expected pet errors ---\n");

    /* unknown keys and a missing trailing newline are fine */
    const char *path = path_in_dir("extra.state");
    write_file(path, "health=50\nclean_exit=1\nt=123\nasleep=1");
    CHECK(pet_load(&p, path) == 0);
    CHECK(p.health == 50 && p.asleep);
    CHECK(unlink(path) == 0);
}

/* ---------- rules ----------
 * Written before pet_update existed, from the code plan and the decisions
 * agreed for Step 3 (see pet.h). Thresholds are the conf defaults:
 * CO2 1000/1500, VOC 150/250, dB 70/85, temp 18-26, RH 30-60, 20 steps,
 * motion 0.5, proximity 200, light 10, sleep 30 s, wake motion 1.0,
 * happy >= 70, ok >= 40. Each pet_update call's readings count for dt
 * seconds, so 30 calls with dt = 1 are 30 s.
 *
 * These FAIL until pet_update / pet_tap / pet_worst are implemented.
 */

/* Readings with every value missing. */
static struct readings none(void)
{
    struct readings r;
    readings_clear(&r);
    return r;
}

/* n one-second cycles with the same readings. */
static void run(struct pet *p, struct readings r, int n)
{
    for (int i = 0; i < n; i++)
        pet_update(p, &r, 1.0);
}

/* Health after one minute of r, starting from health 80. */
static int one_minute(struct readings r)
{
    struct pet p;
    pet_init(&p, &conf);
    run(&p, r, 60);
    return p.health;
}

static void test_single_rules(void)
{
    struct readings r;

    CHECK(one_minute(none()) == 80); /* nothing measured: nothing changes */

    r = none(); r.co2 = 1200; CHECK(one_minute(r) == 79);
    r = none(); r.co2 = 1600; CHECK(one_minute(r) == 78);
    r = none(); r.voc = 200;  CHECK(one_minute(r) == 79);
    r = none(); r.voc = 300;  CHECK(one_minute(r) == 78);
    r = none(); r.db = 75;    CHECK(one_minute(r) == 79);
    r = none(); r.db = 90;    CHECK(one_minute(r) == 78);

    r = none(); r.temp_c = 30; CHECK(one_minute(r) == 79);
    r = none(); r.temp_c = 10; CHECK(one_minute(r) == 79);
    r = none(); r.rh = 70;     CHECK(one_minute(r) == 79);
    r = none(); r.rh = 20;     CHECK(one_minute(r) == 79);
    r = none(); r.temp_c = 30; r.rh = 70; CHECK(one_minute(r) == 79); /* -1 once */
    r = none(); r.temp_c = 22; r.rh = 45; CHECK(one_minute(r) == 80);

    r = none(); r.motion = 0.6; CHECK(one_minute(r) == 81);
    r = none(); r.motion = 0.5; CHECK(one_minute(r) == 80); /* not above */

    /* clean air: co2 and voc that have samples are all <= WARN */
    r = none(); r.co2 = 800; r.voc = 100; CHECK(one_minute(r) == 81);
    r = none(); r.voc = 100;              CHECK(one_minute(r) == 81);
    r = none(); r.co2 = 1000;             CHECK(one_minute(r) == 81); /* not above WARN */
    r = none(); r.co2 = 800; r.voc = 200; CHECK(one_minute(r) == 79); /* voc -1, no bonus */
    r = none(); r.db = 50;                CHECK(one_minute(r) == 80); /* noise is not air */
}

static void test_steps(void)
{
    struct pet p;
    struct readings r = none();

    /* 25 new steps over the minute: +1 */
    pet_init(&p, &conf);
    for (int i = 0; i < 60; i++) {
        r.steps = 100 + (i * 25) / 59;
        pet_update(&p, &r, 1.0);
    }
    CHECK(p.health == 81);

    /* 10 new steps: no bonus */
    pet_init(&p, &conf);
    for (int i = 0; i < 60; i++) {
        r.steps = 100 + (i * 10) / 59;
        pet_update(&p, &r, 1.0);
    }
    CHECK(p.health == 80);

    /* a sensor reset (count goes backwards) is not a big negative or positive */
    pet_init(&p, &conf);
    r.steps = 500;
    run(&p, r, 30);
    r.steps = 3;
    run(&p, r, 30);
    CHECK(p.health == 80);
}

static void test_sum_and_minute(void)
{
    struct pet p;
    struct readings r = none();

    /* -2 co2, -2 voc, -2 db, -1 temp, +1 motion = -6 */
    r.co2 = 1600; r.voc = 300; r.db = 90; r.temp_c = 30; r.motion = 0.6;
    CHECK(one_minute(r) == 74);

    /* applied once per minute, at the minute */
    pet_init(&p, &conf);
    r = none(); r.co2 = 1600;
    run(&p, r, 59);
    CHECK(p.health == 80);
    run(&p, r, 1);
    CHECK(p.health == 78);
    run(&p, r, 60);
    CHECK(p.health == 76);

    /* dt counts, not the number of calls */
    pet_init(&p, &conf);
    for (int i = 0; i < 30; i++)
        pet_update(&p, &r, 2.0);
    CHECK(p.health == 78);
}

static void test_averages_and_nulls(void)
{
    struct pet p;
    struct readings r;

    /* minute average 1050 > 1000: -1 */
    pet_init(&p, &conf);
    r = none(); r.co2 = 900;  run(&p, r, 30);
    r = none(); r.co2 = 1200; run(&p, r, 30);
    CHECK(p.health == 79);

    /* one spike does not cost health: average ~968, clean air +1 */
    pet_init(&p, &conf);
    r = none(); r.co2 = 900;  run(&p, r, 59);
    r = none(); r.co2 = 5000; run(&p, r, 1);
    CHECK(p.health == 81);

    /* nulls are skipped, not counted as zero: average of the 10 samples */
    pet_init(&p, &conf);
    r = none(); r.co2 = 1200; run(&p, r, 10);
    run(&p, none(), 50);
    CHECK(p.health == 79);
}

static void test_clamp(void)
{
    struct pet p;
    struct readings r = none();

    pet_init(&p, &conf);
    p.health = 1;
    r.co2 = 1600; r.voc = 300;
    run(&p, r, 60);
    CHECK(p.health == 0);
    run(&p, r, 60);
    CHECK(p.health == 0);

    pet_init(&p, &conf);
    p.health = 100;
    r = none(); r.co2 = 500; r.motion = 0.6;
    run(&p, r, 60);
    CHECK(p.health == 100);
}

static enum pet_mood mood_at(int health)
{
    struct pet p;
    pet_init(&p, &conf);
    p.health = health;
    run(&p, none(), 1);
    return p.mood;
}

static void test_mood(void)
{
    CHECK(mood_at(100) == PET_HAPPY);
    CHECK(mood_at(70) == PET_HAPPY);
    CHECK(mood_at(69) == PET_OK);
    CHECK(mood_at(40) == PET_OK);
    CHECK(mood_at(39) == PET_SICK);
    CHECK(mood_at(0) == PET_SICK);
}

static void test_sleep(void)
{
    struct pet p;
    struct readings covered = none(), dark = none(), r;
    covered.proximity = 250;
    dark.light = 5;

    /* covered for 30 s straight: asleep */
    pet_init(&p, &conf);
    run(&p, covered, 29);
    CHECK(!p.asleep && p.mood != PET_SLEEPING);
    run(&p, covered, 1);
    CHECK(p.asleep && p.mood == PET_SLEEPING);

    /* dark for 30 s: asleep */
    pet_init(&p, &conf);
    run(&p, dark, 30);
    CHECK(p.asleep);

    /* thresholds are strict */
    pet_init(&p, &conf);
    r = none(); r.proximity = 200; run(&p, r, 40);
    CHECK(!p.asleep);
    r = none(); r.light = 10; run(&p, r, 40);
    CHECK(!p.asleep);

    /* must be continuous */
    pet_init(&p, &conf);
    run(&p, covered, 20);
    run(&p, none(), 1);
    run(&p, covered, 20);
    CHECK(!p.asleep);

    /* rules pause while asleep: 3 min of bad air, asleep after 30 s */
    pet_init(&p, &conf);
    r = covered; r.co2 = 1600;
    run(&p, r, 180);
    CHECK(p.asleep && p.health == 80);

    /* stays asleep without being covered; only a tap or motion wakes it */
    run(&p, none(), 60);
    CHECK(p.asleep);

    /* tap wakes it, and the sleep timer starts over */
    pet_tap(&p);
    CHECK(!p.asleep);
    run(&p, none(), 1);
    CHECK(p.mood == PET_HAPPY);
    run(&p, covered, 29);
    CHECK(!p.asleep);
    run(&p, covered, 1);
    CHECK(p.asleep);

    /* motion above WAKE wakes it; at WAKE it does not */
    r = none(); r.motion = 1.0; run(&p, r, 1);
    CHECK(p.asleep);
    r.motion = 1.5; run(&p, r, 1);
    CHECK(!p.asleep && p.mood != PET_SLEEPING);

    /* after waking, the minute starts over: the 30 s awake before the nap do
     * not carry over, so bad air costs health only once a full minute has
     * passed since waking. The waking call itself is not a sample: its
     * motion must not also count as activity (+1) for the new minute. */
    r = none(); r.co2 = 1600; run(&p, r, 58);
    CHECK(p.health == 80);
    run(&p, r, 4);
    CHECK(p.health == 78);

    /* a pet loaded asleep stays asleep */
    pet_init(&p, &conf);
    p.asleep = true;
    run(&p, none(), 5);
    CHECK(p.asleep && p.mood == PET_SLEEPING);

    /* a tap while awake is harmless */
    pet_init(&p, &conf);
    pet_tap(&p);
    CHECK(!p.asleep);
}

static int worst_is(struct readings r, const char *want)
{
    struct pet p;
    pet_init(&p, &conf);
    const char *got = pet_worst(&p, &r);
    if (want == NULL)
        return got == NULL;
    return got != NULL && strcmp(got, want) == 0;
}

static void test_worst(void)
{
    struct readings r;

    CHECK(worst_is(none(), NULL));
    r = none(); r.co2 = 500; r.voc = 100; r.db = 40; r.temp_c = 22; r.rh = 45;
    CHECK(worst_is(r, NULL));

    r = none(); r.co2 = 1200;  CHECK(worst_is(r, "CO\xe2\x82\x82 high"));
    r = none(); r.voc = 200;   CHECK(worst_is(r, "Air quality poor"));
    r = none(); r.db = 75;     CHECK(worst_is(r, "Too noisy"));
    r = none(); r.temp_c = 10; CHECK(worst_is(r, "Too cold"));
    r = none(); r.temp_c = 30; CHECK(worst_is(r, "Too hot"));
    r = none(); r.rh = 80;     CHECK(worst_is(r, "Too humid"));
    r = none(); r.rh = 10;     CHECK(worst_is(r, "Too dry"));

    /* biggest penalty wins */
    r = none(); r.co2 = 1200; r.voc = 300; CHECK(worst_is(r, "Air quality poor"));
    r = none(); r.temp_c = 30; r.db = 90;  CHECK(worst_is(r, "Too noisy"));
    /* ties go in the order CO2, VOC, noise, temp, RH */
    r = none(); r.co2 = 1600; r.voc = 300; CHECK(worst_is(r, "CO\xe2\x82\x82 high"));
    r = none(); r.co2 = 1200; r.temp_c = 30; CHECK(worst_is(r, "CO\xe2\x82\x82 high"));
    r = none(); r.temp_c = 30; r.rh = 80;  CHECK(worst_is(r, "Too hot"));
}

int main(void)
{
    if (mkdtemp(dir) == NULL) {
        perror("mkdtemp");
        return 2;
    }
    conf_defaults(&conf);

    test_defaults();
    test_expand_path();
    test_round_trip();
    test_bad_files();

    test_single_rules();
    test_steps();
    test_sum_and_minute();
    test_averages_and_nulls();
    test_clamp();
    test_mood();
    test_sleep();
    test_worst();

    if (rmdir(dir) != 0)
        perror("rmdir test dir");
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
