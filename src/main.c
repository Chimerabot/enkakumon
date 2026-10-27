/*
 * main.c — enkakumond, the 1 Hz sensor daemon.
 *
 * Usage: enkakumond [--print] [conf]      (conf defaults to enkakumon.conf)
 * Exit:  0 clean stop on SIGTERM/SIGINT, 1 config error,
 *        2 socket, timer or another non-sensor resource is unusable.
 *
 * Acceptance tests (on the Pi):
 *   make && make test && make asan pass.
 *   ./enkakumond --print prints values every second for 2 minutes without
 *   crashing; breathing raises co2, covering the APDS9960 raises proximity,
 *   shaking raises motion, clapping raises db.
 *   With I2C disconnected it prints -- and keeps running.
 *   socat - UNIX-CONNECT:<SOCKET_PATH> shows one JSON line per second.
 *   systemctl stop enkakumon exits 0. make soakcheck passes.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "conf.h"
#include "ipc.h"
#include "mic.h"
#include "sensors.h"

#define EXIT_CONFIG 1
#define EXIT_RESOURCE 2
#define CYCLE_BUDGET_NS 100000000LL

struct daemon {
    struct conf conf;
    struct sensors sensors;
    struct mic mic;
    struct ipc ipc;
    bool print;
    int sig_fd, timer_fd, hb_fd;
    bool hb_logged, json_logged, print_logged;
};

static struct daemon d; /* large; keep it off the stack */

static long long elapsed_ns(const struct timespec *a, const struct timespec *b)
{
    return (long long)(b->tv_sec - a->tv_sec) * 1000000000LL + (b->tv_nsec - a->tv_nsec);
}

static void print_val(const char *name, double v, int decimals, bool first)
{
    printf("%s%s=", first ? "" : " ", name);
    if (isfinite(v))
        printf("%.*f", decimals, v);
    else
        fputs("--", stdout);
}

static void print_line(const struct readings *r)
{
    print_val("co2", r->co2, 0, true);
    print_val("temp_c", r->temp_c, 1, false);
    print_val("rh", r->rh, 1, false);
    print_val("voc", r->voc, 0, false);
    print_val("light", r->light, 0, false);
    print_val("proximity", r->proximity, 0, false);
    print_val("motion", r->motion, 2, false);
    print_val("steps", r->steps, 0, false);
    print_val("db", r->db, 1, false);
    putchar('\n');
    if (fflush(stdout) != 0 && !d.print_logged) {
        fprintf(stderr, "enkakumond: stdout: %s\n", strerror(errno));
        d.print_logged = true;
    }
}

static void cycle(void)
{
    struct timespec t0, t1;
    struct readings r;
    char line[512];

    clock_gettime(CLOCK_MONOTONIC, &t0);
    readings_clear(&r);

    sensors_read(&d.sensors, &r, t0.tv_sec);
    mic_poll(&d.mic, t0.tv_sec);
    r.db = mic_db(&d.mic);
    /* analysis: Step 3 */

    int len = ipc_format_json(line, sizeof(line), time(NULL), &r);
    if (len > 0) {
        ipc_publish(&d.ipc, line, (size_t)len);
    } else if (!d.json_logged) {
        fprintf(stderr, "enkakumond: ipc: JSON line did not fit\n");
        d.json_logged = true;
    }
    if (d.print)
        print_line(&r);

    if (futimens(d.hb_fd, NULL) != 0) {
        if (!d.hb_logged) {
            fprintf(stderr, "enkakumond: heartbeat: %s\n", strerror(errno));
            d.hb_logged = true;
        }
    } else {
        d.hb_logged = false;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long ns = elapsed_ns(&t0, &t1);
    if (ns > CYCLE_BUDGET_NS)
        fprintf(stderr, "enkakumond: cycle took %lld ms (budget 100 ms)\n", ns / 1000000);
}

/* Returns the exit code. */
static int run(void)
{
    struct pollfd pfd[3 + IPC_MAX_CLIENTS];
    int slot_of[3 + IPC_MAX_CLIENTS];

    for (;;) {
        int n = 0;
        pfd[n++] = (struct pollfd){ .fd = d.sig_fd, .events = POLLIN };
        pfd[n++] = (struct pollfd){ .fd = d.timer_fd, .events = POLLIN };
        pfd[n++] = (struct pollfd){ .fd = d.ipc.listen_fd, .events = POLLIN };
        for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
            if (d.ipc.clients[i].fd >= 0) {
                pfd[n] = (struct pollfd){ .fd = d.ipc.clients[i].fd, .events = POLLIN };
                slot_of[n++] = i;
            }
        }

        if (poll(pfd, (nfds_t)n, -1) < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "enkakumond: poll: %s\n", strerror(errno));
            return EXIT_RESOURCE;
        }

        if (pfd[0].revents & POLLIN) {
            struct signalfd_siginfo si;
            ssize_t k = read(d.sig_fd, &si, sizeof(si));
            if (k == (ssize_t)sizeof(si)) {
                fprintf(stderr, "enkakumond: %s, stopping\n", strsignal((int)si.ssi_signo));
                return 0;
            }
            if (k < 0 && errno != EAGAIN) {
                fprintf(stderr, "enkakumond: signalfd read: %s\n", strerror(errno));
                return EXIT_RESOURCE;
            }
        }

        if (pfd[1].revents & POLLIN) {
            uint64_t ticks;
            ssize_t k = read(d.timer_fd, &ticks, sizeof(ticks));
            if (k == (ssize_t)sizeof(ticks)) {
                if (ticks > 1)
                    fprintf(stderr, "enkakumond: missed %llu timer ticks\n",
                            (unsigned long long)(ticks - 1));
                cycle();
            } else if (k < 0 && errno != EAGAIN) {
                fprintf(stderr, "enkakumond: timerfd read: %s\n", strerror(errno));
                return EXIT_RESOURCE;
            }
        }

        /* Clients before accept, so a dropped slot's fd number cannot have
         * been reused by a new client within this iteration. */
        for (int j = 3; j < n; j++) {
            int slot = slot_of[j];
            if (d.ipc.clients[slot].fd != pfd[j].fd)
                continue; /* dropped during publish */
            if (pfd[j].revents & POLLIN)
                ipc_client_input(&d.ipc, slot);
            else if (pfd[j].revents & (POLLERR | POLLHUP | POLLNVAL))
                ipc_drop(&d.ipc, slot);
        }

        if (pfd[2].revents & POLLIN)
            ipc_accept(&d.ipc);
    }
}

static void close_fd(int *fd, const char *what)
{
    if (*fd < 0)
        return;
    if (close(*fd) != 0)
        fprintf(stderr, "enkakumond: close %s: %s\n", what, strerror(errno));
    *fd = -1;
}

static int usage(void)
{
    fprintf(stderr, "usage: enkakumond [--print] [conf]\n");
    return EXIT_CONFIG;
}

int main(int argc, char **argv)
{
    const char *conf_path = "enkakumon.conf";
    bool conf_given = false;
    bool ipc_ok = false, mic_ok = false, sensors_ok = false;
    int rc = EXIT_RESOURCE;

    d.sig_fd = d.timer_fd = d.hb_fd = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--print") == 0)
            d.print = true;
        else if (argv[i][0] == '-' || conf_given)
            return usage();
        else {
            conf_path = argv[i];
            conf_given = true;
        }
    }

    conf_defaults(&d.conf);
    if (conf_load(&d.conf, conf_path) != 0)
        return EXIT_CONFIG;

    /* Block the stop signals before any thread exists so all threads inherit
     * the mask and they are only ever delivered through the signalfd. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    if (sigprocmask(SIG_BLOCK, &set, NULL) != 0) {
        fprintf(stderr, "enkakumond: sigprocmask: %s\n", strerror(errno));
        goto cleanup;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        fprintf(stderr, "enkakumond: ignoring SIGPIPE: %s\n", strerror(errno));
        goto cleanup;
    }
    d.sig_fd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    if (d.sig_fd < 0) {
        fprintf(stderr, "enkakumond: signalfd: %s\n", strerror(errno));
        goto cleanup;
    }

    d.hb_fd = open(d.conf.heartbeat_path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (d.hb_fd < 0) {
        fprintf(stderr, "enkakumond: heartbeat %s: %s\n", d.conf.heartbeat_path, strerror(errno));
        goto cleanup;
    }
    if (futimens(d.hb_fd, NULL) != 0) {
        fprintf(stderr, "enkakumond: heartbeat %s: %s\n", d.conf.heartbeat_path, strerror(errno));
        goto cleanup;
    }

    d.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (d.timer_fd < 0) {
        fprintf(stderr, "enkakumond: timerfd_create: %s\n", strerror(errno));
        goto cleanup;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct itimerspec its = {
        .it_interval = { .tv_sec = 1, .tv_nsec = 0 },
        .it_value = { .tv_sec = now.tv_sec + 1, .tv_nsec = now.tv_nsec },
    };
    if (timerfd_settime(d.timer_fd, TFD_TIMER_ABSTIME, &its, NULL) != 0) {
        fprintf(stderr, "enkakumond: timerfd_settime: %s\n", strerror(errno));
        goto cleanup;
    }

    if (ipc_open(&d.ipc, &d.conf) != 0)
        goto cleanup;
    ipc_ok = true;

    sensors_init(&d.sensors, &d.conf);
    sensors_ok = true;

    if (mic_init(&d.mic, &d.conf) != 0)
        goto cleanup;
    mic_ok = true;

    fprintf(stderr, "enkakumond: running (conf %s, socket %s)\n", conf_path, d.conf.socket_path);
    rc = run();

cleanup:
    if (sensors_ok)
        sensors_close(&d.sensors);
    if (mic_ok)
        mic_close(&d.mic);
    if (ipc_ok)
        ipc_close(&d.ipc);
    close_fd(&d.timer_fd, "timerfd");
    close_fd(&d.hb_fd, "heartbeat");
    close_fd(&d.sig_fd, "signalfd");
    return rc;
}
