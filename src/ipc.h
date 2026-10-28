/*
 * ipc.h — local Unix stream socket: one JSON line per second to every client.
 *
 * Sends never block: a client whose send would block, or is short, is dropped.
 * Clients may send "tap\n"; anything else is ignored. Connections beyond
 * MAX_CLIENTS are accepted and closed at once.
 *
 * Acceptance tests (test/test_main.c):
 *   the JSON line matches the contract with all values and with nulls;
 *   "tap\n" is counted and other input ignored; a closed client is dropped.
 * On the Pi: socat - UNIX-CONNECT:<SOCKET_PATH> shows one JSON line per second.
 */
#ifndef ENKAKUMON_IPC_H
#define ENKAKUMON_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "conf.h"
#include "sensors.h"

#define IPC_MAX_CLIENTS 64
#define IPC_LINE_MAX 64

struct ipc_client {
    int fd; /* -1 when the slot is free */
    size_t in_len;
    bool discarding; /* current input line overflowed; skip to newline */
    char in[IPC_LINE_MAX];
};

struct ipc {
    int listen_fd;
    int max_clients;
    char path[CONF_STR_MAX];
    unsigned taps;
    struct ipc_client clients[IPC_MAX_CLIENTS];
};

/* Sets every slot free without opening anything (also used by tests). */
void ipc_reset(struct ipc *p, int max_clients);

/* Returns 0, or -1 after logging (the daemon exits 2). */
int ipc_open(struct ipc *p, const struct conf *c);

void ipc_accept(struct ipc *p);
void ipc_client_input(struct ipc *p, int slot);
void ipc_drop(struct ipc *p, int slot);
void ipc_publish(struct ipc *p, const char *line, size_t len);
void ipc_close(struct ipc *p);

/* Formats the contract line, newline included. Returns its length, or -1. */
int ipc_format_json(char *buf, size_t size, time_t t, const struct readings *r);

#endif
