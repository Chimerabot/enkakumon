/*
 * test_pet.c — unit tests for the health system (src/analysis/pet.c).
 * Run from the repo root by make test, make asan and make memcheck.
 *
 * Covers: defaults, path expansion, save/load round-trip, missing and
 * corrupt state files, and that a save leaves no .tmp behind.
 * "pet: ... using defaults" lines printed during the run are expected.
 */
#include <errno.h>
#include <limits.h>
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

    if (rmdir(dir) != 0)
        perror("rmdir test dir");
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
