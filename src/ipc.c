#include "ipc.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

void ipc_reset(struct ipc *p, int max_clients)
{
    memset(p, 0, sizeof(*p));
    p->listen_fd = -1;
    p->max_clients = max_clients > IPC_MAX_CLIENTS ? IPC_MAX_CLIENTS : max_clients;
    for (int i = 0; i < IPC_MAX_CLIENTS; i++)
        p->clients[i].fd = -1;
}

static int fill_addr(struct sockaddr_un *addr, const char *path)
{
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr->sun_path))
        return -1;
    strcpy(addr->sun_path, path);
    return 0;
}

/* A leftover socket file may be removed; a live daemon or a non-socket may not.
 * Logs its own errors. */
static int clear_stale(const char *path, const struct sockaddr_un *addr)
{
    struct stat st;

    if (lstat(path, &st) != 0) {
        if (errno == ENOENT)
            return 0;
        fprintf(stderr, "enkakumond: ipc: lstat %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (!S_ISSOCK(st.st_mode)) {
        fprintf(stderr, "enkakumond: ipc: %s exists and is not a socket\n", path);
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fprintf(stderr, "enkakumond: ipc: socket: %s\n", strerror(errno));
        return -1;
    }
    int live = connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) == 0;
    if (close(fd) != 0)
        fprintf(stderr, "enkakumond: ipc: close probe failed: %s\n", strerror(errno));
    if (live) {
        fprintf(stderr, "enkakumond: ipc: another daemon is serving %s\n", path);
        return -1;
    }
    if (unlink(path) != 0) {
        fprintf(stderr, "enkakumond: ipc: unlink %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

int ipc_open(struct ipc *p, const struct conf *c)
{
    struct sockaddr_un addr;

    ipc_reset(p, c->max_clients);
    if (fill_addr(&addr, c->socket_path) != 0) {
        fprintf(stderr, "enkakumond: ipc: SOCKET_PATH too long\n");
        return -1;
    }
    strcpy(p->path, c->socket_path);

    if (clear_stale(p->path, &addr) != 0)
        return -1;

    p->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (p->listen_fd < 0) {
        fprintf(stderr, "enkakumond: ipc: socket: %s\n", strerror(errno));
        return -1;
    }
    if (bind(p->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "enkakumond: ipc: bind %s: %s\n", p->path, strerror(errno));
        goto fail;
    }
    if (listen(p->listen_fd, 8) != 0) {
        fprintf(stderr, "enkakumond: ipc: listen: %s\n", strerror(errno));
        if (unlink(p->path) != 0)
            fprintf(stderr, "enkakumond: ipc: unlink %s: %s\n", p->path, strerror(errno));
        goto fail;
    }
    return 0;

fail:
    if (close(p->listen_fd) != 0)
        fprintf(stderr, "enkakumond: ipc: close: %s\n", strerror(errno));
    p->listen_fd = -1;
    return -1;
}

void ipc_accept(struct ipc *p)
{
    for (;;) {
        int fd = accept4(p->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                fprintf(stderr, "enkakumond: ipc: accept: %s\n", strerror(errno));
            return;
        }

        int slot = -1;
        for (int i = 0; i < p->max_clients; i++) {
            if (p->clients[i].fd < 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            fprintf(stderr, "enkakumond: ipc: client limit (%d) reached, refusing\n",
                    p->max_clients);
            if (close(fd) != 0)
                fprintf(stderr, "enkakumond: ipc: close: %s\n", strerror(errno));
            continue;
        }
        struct ipc_client *cl = &p->clients[slot];
        cl->fd = fd;
        cl->in_len = 0;
        cl->discarding = false;
    }
}

void ipc_drop(struct ipc *p, int slot)
{
    struct ipc_client *cl = &p->clients[slot];

    if (cl->fd < 0)
        return;
    if (close(cl->fd) != 0)
        fprintf(stderr, "enkakumond: ipc: close client: %s\n", strerror(errno));
    cl->fd = -1;
}

static void handle_line(struct ipc *p, const char *line, size_t len)
{
    if (len > 0 && line[len - 1] == '\r')
        len--;
    if (len == 3 && memcmp(line, "tap", 3) == 0)
        p->taps++;
}

void ipc_client_input(struct ipc *p, int slot)
{
    struct ipc_client *cl = &p->clients[slot];
    char buf[256];

    ssize_t n = recv(cl->fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n == 0) {
        ipc_drop(p, slot);
        return;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            ipc_drop(p, slot);
        return;
    }

    for (ssize_t i = 0; i < n; i++) {
        char ch = buf[i];
        if (ch == '\n') {
            if (!cl->discarding)
                handle_line(p, cl->in, cl->in_len);
            cl->in_len = 0;
            cl->discarding = false;
        } else if (!cl->discarding) {
            if (cl->in_len == sizeof(cl->in)) {
                cl->discarding = true;
                cl->in_len = 0;
            } else {
                cl->in[cl->in_len++] = ch;
            }
        }
    }
}

void ipc_publish(struct ipc *p, const char *line, size_t len)
{
    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        struct ipc_client *cl = &p->clients[i];
        if (cl->fd < 0)
            continue;
        ssize_t n = send(cl->fd, line, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 || (size_t)n != len)
            ipc_drop(p, i); /* slow or broken: never wait on it */
    }
}

void ipc_close(struct ipc *p)
{
    for (int i = 0; i < IPC_MAX_CLIENTS; i++)
        ipc_drop(p, i);
    if (p->listen_fd >= 0) {
        if (close(p->listen_fd) != 0)
            fprintf(stderr, "enkakumond: ipc: close: %s\n", strerror(errno));
        p->listen_fd = -1;
        if (unlink(p->path) != 0)
            fprintf(stderr, "enkakumond: ipc: unlink %s: %s\n", p->path, strerror(errno));
    }
}

static const char *num(char *dst, size_t size, double v, int decimals)
{
    if (!isfinite(v))
        return "null";
    snprintf(dst, size, "%.*f", decimals, v);
    return dst;
}

int ipc_format_json(char *buf, size_t size, time_t t, const struct readings *r)
{
    char b[9][32];

    int n = snprintf(buf, size,
                     "{\"t\":%lld,\"co2\":%s,\"temp_c\":%s,\"rh\":%s,\"voc\":%s,"
                     "\"light\":%s,\"proximity\":%s,\"motion\":%s,\"steps\":%s,\"db\":%s}\n",
                     (long long)t,
                     num(b[0], sizeof(b[0]), r->co2, 0),
                     num(b[1], sizeof(b[1]), r->temp_c, 1),
                     num(b[2], sizeof(b[2]), r->rh, 1),
                     num(b[3], sizeof(b[3]), r->voc, 0),
                     num(b[4], sizeof(b[4]), r->light, 0),
                     num(b[5], sizeof(b[5]), r->proximity, 0),
                     num(b[6], sizeof(b[6]), r->motion, 2),
                     num(b[7], sizeof(b[7]), r->steps, 0),
                     num(b[8], sizeof(b[8]), r->db, 1));
    if (n < 0 || (size_t)n >= size)
        return -1;
    return n;
}
