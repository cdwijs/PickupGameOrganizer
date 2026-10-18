// netshim: in-memory loopback sockets for the WebAssembly build. See netshim.h.
//
// These definitions replace Emscripten's libc versions at link time (an
// object file wins over the libc.a member). Non-shim fds are handed to the
// same __wasi_* / __syscall_* entry points that Emscripten's libc uses.

#define _GNU_SOURCE
#include "netshim.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <wasi/api.h>

int __syscall_ioctl(int fd, int request, ...);
int __syscall_poll(intptr_t fds, int nfds, int timeout);
int __syscall_fcntl64(int fd, int cmd, ...);

#define RX_CAP (256 * 1024) // per-socket receive buffer; writers see EAGAIN beyond it
#define BACKLOG_MAX 64
#define EPHEMERAL_FIRST 40000
#define LOOPBACK_ADDR 0x7f000001u

enum kind { K_FREE, K_NEW, K_LISTEN, K_STREAM, K_PIPE_R, K_PIPE_W };

typedef struct {
    char *data;
    size_t off, len, cap;
} nbuf;

typedef struct {
    int kind;
    int family;    // AF_INET or AF_UNIX
    int nonblock;
    int peer;      // index of the other end, or -1
    int eof;       // nothing more will arrive (peer closed or shut down writing)
    int peer_gone; // the other end is closed: writes fail with EPIPE
    int rd_shut, wr_shut;
    int so_error;
    nbuf rx;
    uint16_t lport, rport;
    int bound;
    int queue[BACKLOG_MAX]; // K_LISTEN: server ends waiting for accept()
    int qn;
    netshim_notify_fn fn;
    void *ctx;
} sock;

static sock S[NETSHIM_MAX_FDS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static unsigned gen; // bumped on every readiness change, under mu
static uint16_t next_ephemeral = EPHEMERAL_FIRST;

// --- helpers (all called with mu held unless noted) ---------------------------

static int fail(int err)
{
    errno = err;
    return -1;
}

static int wasi_ret(__wasi_errno_t r)
{
    return r ? fail(r) : 0;
}

static int sys_ret(int r)
{
    return (r < 0 && r > -4096) ? fail(-r) : r;
}

static int in_range(int fd)
{
    return fd >= NETSHIM_FD_BASE && fd < NETSHIM_FD_BASE + NETSHIM_MAX_FDS;
}

// Index of an open shim fd, or -1.
static int idx(int fd)
{
    if (!in_range(fd))
        return -1;
    const int i = fd - NETSHIM_FD_BASE;
    return S[i].kind == K_FREE ? -1 : i;
}

static void changed(int i)
{
    gen++;
    pthread_cond_broadcast(&cv);
    if (i >= 0 && S[i].fn)
        S[i].fn(i + NETSHIM_FD_BASE, S[i].ctx);
}

static int alloc(int kind, int family, int nonblock)
{
    for (int i = 0; i < NETSHIM_MAX_FDS; i++) {
        if (S[i].kind != K_FREE)
            continue;
        memset(&S[i], 0, sizeof S[i]);
        S[i].kind = kind;
        S[i].family = family;
        S[i].nonblock = nonblock;
        S[i].peer = -1;
        return i;
    }
    return -1;
}

static void release(int i)
{
    free(S[i].rx.data);
    memset(&S[i], 0, sizeof S[i]);
    S[i].kind = K_FREE;
    S[i].peer = -1;
}

static int port_in_use(uint16_t port)
{
    for (int i = 0; i < NETSHIM_MAX_FDS; i++)
        if (S[i].kind != K_FREE && S[i].bound && S[i].lport == port)
            return 1;
    return 0;
}

static uint16_t ephemeral_port(void)
{
    for (int tries = 0; tries < 20000; tries++) {
        const uint16_t p = next_ephemeral++;
        if (next_ephemeral >= 60000)
            next_ephemeral = EPHEMERAL_FIRST;
        if (!port_in_use(p))
            return p;
    }
    return 0;
}

static int nbuf_append(nbuf *b, const void *src, size_t n)
{
    if (b->off && b->off + b->len + n > b->cap) {
        memmove(b->data, b->data + b->off, b->len);
        b->off = 0;
    }
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n)
            cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p)
            return -1;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->off + b->len, src, n);
    b->len += n;
    return 0;
}

// Wait for a readiness change; returns ETIMEDOUT when the deadline passes.
static int wait_change(const struct timespec *deadline)
{
    const unsigned seen = gen;
    int r = 0;
    while (gen == seen && r == 0)
        r = deadline ? pthread_cond_timedwait(&cv, &mu, deadline) : pthread_cond_wait(&cv, &mu);
    return r;
}

static void deadline_in(struct timespec *ts, int ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static void fill_inet(struct sockaddr *addr, socklen_t *len, uint16_t port)
{
    if (!addr || !len)
        return;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = htonl(LOOPBACK_ADDR);
    const socklen_t n = *len < sizeof sin ? *len : (socklen_t)sizeof sin;
    memcpy(addr, &sin, n);
    *len = sizeof sin;
}

static void fill_unix(struct sockaddr *addr, socklen_t *len)
{
    if (!addr || !len)
        return;
    const sa_family_t family = AF_UNIX;
    const socklen_t n = *len < sizeof family ? *len : (socklen_t)sizeof family;
    memcpy(addr, &family, n);
    *len = sizeof family;
}

// Readiness of one shim socket, as poll() revents.
static short revents_of(int i, short events)
{
    const sock *s = &S[i];
    short r = 0;
    switch (s->kind) {
    case K_LISTEN:
        if (s->qn > 0)
            r |= POLLIN;
        break;
    case K_STREAM:
    case K_PIPE_R:
    case K_PIPE_W:
        if (s->kind != K_PIPE_W && (s->rx.len > 0 || s->eof || s->rd_shut))
            r |= POLLIN;
        if (s->kind != K_PIPE_R) {
            if (s->peer_gone)
                r |= POLLOUT | POLLERR;
            else if (s->peer >= 0 && S[s->peer].rx.len < RX_CAP)
                r |= POLLOUT;
        }
        if (s->peer_gone)
            r |= POLLHUP;
        break;
    case K_NEW:
        r |= POLLOUT;
        break;
    }
    return r & (events | POLLERR | POLLHUP);
}

static ssize_t do_recv(int i, void *buf, size_t len, int flags)
{
    sock *s = &S[i];
    if (s->kind != K_STREAM && s->kind != K_PIPE_R)
        return fail(s->kind == K_PIPE_W ? EBADF : ENOTCONN);
    while (s->rx.len == 0) {
        if (s->eof || s->rd_shut || len == 0)
            return 0;
        if (s->nonblock || (flags & MSG_DONTWAIT))
            return fail(EAGAIN);
        wait_change(NULL);
    }
    const size_t n = len < s->rx.len ? len : s->rx.len;
    memcpy(buf, s->rx.data + s->rx.off, n);
    if (!(flags & MSG_PEEK)) {
        s->rx.off += n;
        s->rx.len -= n;
        if (s->rx.len == 0)
            s->rx.off = 0;
        changed(s->peer); // the writer may have room again
    }
    return (ssize_t)n;
}

static ssize_t do_send(int i, const void *buf, size_t len, int flags)
{
    sock *s = &S[i];
    if (s->kind != K_STREAM && s->kind != K_PIPE_W)
        return fail(s->kind == K_PIPE_R ? EBADF : ENOTCONN);
    for (;;) {
        if (s->peer_gone || s->wr_shut || s->peer < 0)
            return fail(EPIPE);
        const size_t room = RX_CAP - S[s->peer].rx.len;
        if (room > 0) {
            const size_t n = len < room ? len : room;
            if (nbuf_append(&S[s->peer].rx, buf, n) < 0)
                return fail(ENOMEM);
            changed(s->peer);
            return (ssize_t)n;
        }
        if (s->nonblock || (flags & MSG_DONTWAIT))
            return fail(EAGAIN);
        wait_change(NULL);
    }
}

static void do_close(int i)
{
    sock *s = &S[i];
    if (s->kind == K_LISTEN) {
        // Connections nobody accepted: their client ends see the peer vanish.
        for (int q = 0; q < s->qn; q++) {
            const int srv = s->queue[q];
            const int cli = S[srv].peer;
            if (cli >= 0) {
                S[cli].peer = -1;
                S[cli].eof = S[cli].peer_gone = 1;
                changed(cli);
            }
            release(srv);
        }
    } else if (s->peer >= 0) {
        const int p = s->peer;
        S[p].peer = -1;
        S[p].eof = S[p].peer_gone = 1;
        changed(p);
    }
    release(i);
    changed(-1);
}

// --- public netshim API ------------------------------------------------------

int netshim_set_notify(int fd, netshim_notify_fn fn, void *ctx)
{
    pthread_mutex_lock(&mu);
    const int i = idx(fd);
    if (i >= 0) {
        S[i].fn = fn;
        S[i].ctx = ctx;
    }
    pthread_mutex_unlock(&mu);
    return i < 0 ? fail(EBADF) : 0;
}

int netshim_is_shim_fd(int fd)
{
    pthread_mutex_lock(&mu);
    const int r = idx(fd) >= 0;
    pthread_mutex_unlock(&mu);
    return r;
}

// --- socket API ----------------------------------------------------------------

int socket(int domain, int type, int protocol)
{
    (void)protocol;
    if (domain != AF_INET)
        return fail(EAFNOSUPPORT);
    if ((type & 0xf) != SOCK_STREAM)
        return fail(EPROTONOSUPPORT);
    pthread_mutex_lock(&mu);
    const int i = alloc(K_NEW, AF_INET, !!(type & SOCK_NONBLOCK));
    pthread_mutex_unlock(&mu);
    return i < 0 ? fail(EMFILE) : i + NETSHIM_FD_BASE;
}

int socketpair(int domain, int type, int protocol, int sv[2])
{
    (void)protocol;
    if (domain != AF_UNIX && domain != AF_INET)
        return fail(EAFNOSUPPORT);
    if ((type & 0xf) != SOCK_STREAM)
        return fail(EPROTONOSUPPORT);
    const int nonblock = !!(type & SOCK_NONBLOCK);
    pthread_mutex_lock(&mu);
    const int a = alloc(K_STREAM, domain, nonblock);
    const int b = a < 0 ? -1 : alloc(K_STREAM, domain, nonblock);
    if (b < 0) {
        if (a >= 0)
            release(a);
        pthread_mutex_unlock(&mu);
        return fail(EMFILE);
    }
    S[a].peer = b;
    S[b].peer = a;
    pthread_mutex_unlock(&mu);
    sv[0] = a + NETSHIM_FD_BASE;
    sv[1] = b + NETSHIM_FD_BASE;
    return 0;
}

int pipe2(int fd[2], int flags)
{
    const int nonblock = !!(flags & O_NONBLOCK);
    pthread_mutex_lock(&mu);
    const int r = alloc(K_PIPE_R, AF_UNIX, nonblock);
    const int w = r < 0 ? -1 : alloc(K_PIPE_W, AF_UNIX, nonblock);
    if (w < 0) {
        if (r >= 0)
            release(r);
        pthread_mutex_unlock(&mu);
        return fail(EMFILE);
    }
    // The write end appends to the read end's buffer, exactly like a stream.
    S[r].peer = w;
    S[w].peer = r;
    pthread_mutex_unlock(&mu);
    fd[0] = r + NETSHIM_FD_BASE;
    fd[1] = w + NETSHIM_FD_BASE;
    return 0;
}

// Emscripten has no eventfd, so tor's alert sockets and libevent's wake-up
// use pipes, which land here.
int pipe(int fd[2])
{
    return pipe2(fd, 0);
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    if (!addr || len < sizeof(struct sockaddr_in) || addr->sa_family != AF_INET)
        return fail(EAFNOSUPPORT);
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    const uint32_t ip = ntohl(sin->sin_addr.s_addr);
    if (ip != LOOPBACK_ADDR && ip != INADDR_ANY)
        return fail(EADDRNOTAVAIL);
    pthread_mutex_lock(&mu);
    int r = 0;
    const int i = idx(fd);
    if (i < 0) {
        r = fail(in_range(fd) ? EBADF : ENOTSOCK);
    } else if (S[i].kind != K_NEW || S[i].bound) {
        r = fail(EINVAL);
    } else {
        uint16_t port = ntohs(sin->sin_port);
        if (port == 0)
            port = ephemeral_port();
        if (port == 0 || port_in_use(port)) {
            r = fail(EADDRINUSE);
        } else {
            S[i].lport = port;
            S[i].bound = 1;
        }
    }
    pthread_mutex_unlock(&mu);
    return r;
}

int listen(int fd, int backlog)
{
    (void)backlog;
    pthread_mutex_lock(&mu);
    int r = 0;
    const int i = idx(fd);
    if (i < 0) {
        r = fail(in_range(fd) ? EBADF : ENOTSOCK);
    } else if (S[i].kind == K_NEW) {
        if (!S[i].bound) {
            S[i].lport = ephemeral_port();
            S[i].bound = 1;
        }
        S[i].kind = K_LISTEN;
    } else if (S[i].kind != K_LISTEN) {
        r = fail(EINVAL);
    }
    pthread_mutex_unlock(&mu);
    return r;
}

int accept4(int fd, struct sockaddr *restrict addr, socklen_t *restrict len, int flags)
{
    pthread_mutex_lock(&mu);
    int r;
    const int i = idx(fd);
    if (i < 0) {
        r = fail(in_range(fd) ? EBADF : ENOTSOCK);
    } else if (S[i].kind != K_LISTEN) {
        r = fail(EINVAL);
    } else {
        r = -1;
        for (;;) {
            if (S[i].qn > 0) {
                const int srv = S[i].queue[0];
                memmove(S[i].queue, S[i].queue + 1, (size_t)--S[i].qn * sizeof S[i].queue[0]);
                S[srv].nonblock = !!(flags & SOCK_NONBLOCK);
                fill_inet(addr, len, S[srv].rport);
                r = srv + NETSHIM_FD_BASE;
                break;
            }
            if (S[i].nonblock) {
                r = fail(EAGAIN);
                break;
            }
            wait_change(NULL);
            if (idx(fd) != i || S[i].kind != K_LISTEN) {
                r = fail(EBADF);
                break;
            }
        }
    }
    pthread_mutex_unlock(&mu);
    return r;
}

int accept(int fd, struct sockaddr *restrict addr, socklen_t *restrict len)
{
    return accept4(fd, addr, len, 0);
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    if (!addr || len < sizeof(struct sockaddr_in) || addr->sa_family != AF_INET)
        return fail(EAFNOSUPPORT);
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    // There is no network in the tab, only loopback (127.0.0.0/8).
    if ((ntohl(sin->sin_addr.s_addr) >> 24) != 127)
        return fail(ENETUNREACH);
    const uint16_t port = ntohs(sin->sin_port);

    pthread_mutex_lock(&mu);
    int r = 0;
    const int i = idx(fd);
    int listener = -1;
    for (int k = 0; k < NETSHIM_MAX_FDS; k++)
        if (S[k].kind == K_LISTEN && S[k].lport == port)
            listener = k;
    if (i < 0) {
        r = fail(in_range(fd) ? EBADF : ENOTSOCK);
    } else if (S[i].kind != K_NEW) {
        r = fail(S[i].kind == K_STREAM ? EISCONN : EINVAL);
    } else if (listener < 0 || S[listener].qn >= BACKLOG_MAX) {
        r = fail(ECONNREFUSED);
    } else {
        const int srv = alloc(K_STREAM, AF_INET, 0);
        if (srv < 0) {
            r = fail(EMFILE);
        } else {
            if (!S[i].bound) {
                S[i].lport = ephemeral_port();
                S[i].bound = 1;
            }
            S[i].kind = K_STREAM;
            S[i].peer = srv;
            S[i].rport = port;
            S[srv].peer = i;
            S[srv].lport = port;
            S[srv].rport = S[i].lport;
            S[listener].queue[S[listener].qn++] = srv;
            changed(listener);
            changed(i);
        }
    }
    pthread_mutex_unlock(&mu);
    return r; // loopback connects complete immediately, even non-blocking
}

int shutdown(int fd, int how)
{
    pthread_mutex_lock(&mu);
    int r = 0;
    const int i = idx(fd);
    if (i < 0) {
        r = fail(in_range(fd) ? EBADF : ENOTSOCK);
    } else if (S[i].kind != K_STREAM) {
        r = fail(ENOTCONN);
    } else {
        if (how == SHUT_RD || how == SHUT_RDWR)
            S[i].rd_shut = 1;
        if ((how == SHUT_WR || how == SHUT_RDWR) && !S[i].wr_shut) {
            S[i].wr_shut = 1;
            if (S[i].peer >= 0) {
                S[S[i].peer].eof = 1;
                changed(S[i].peer);
            }
        }
        changed(i);
    }
    pthread_mutex_unlock(&mu);
    return r;
}

int getsockname(int fd, struct sockaddr *restrict addr, socklen_t *restrict len)
{
    pthread_mutex_lock(&mu);
    int r = 0;
    const int i = idx(fd);
    if (i < 0)
        r = fail(in_range(fd) ? EBADF : ENOTSOCK);
    else if (S[i].family == AF_UNIX)
        fill_unix(addr, len);
    else
        fill_inet(addr, len, S[i].lport);
    pthread_mutex_unlock(&mu);
    return r;
}

int getpeername(int fd, struct sockaddr *restrict addr, socklen_t *restrict len)
{
    pthread_mutex_lock(&mu);
    int r = 0;
    const int i = idx(fd);
    if (i < 0)
        r = fail(in_range(fd) ? EBADF : ENOTSOCK);
    else if (S[i].kind != K_STREAM || S[i].peer < 0)
        r = fail(ENOTCONN);
    else if (S[i].family == AF_UNIX)
        fill_unix(addr, len);
    else
        fill_inet(addr, len, S[i].rport);
    pthread_mutex_unlock(&mu);
    return r;
}

int getsockopt(int fd, int level, int name, void *restrict value, socklen_t *restrict len)
{
    pthread_mutex_lock(&mu);
    int r = 0;
    const int i = idx(fd);
    if (i < 0) {
        r = fail(in_range(fd) ? EBADF : ENOTSOCK);
    } else if (!value || !len || *len < sizeof(int)) {
        r = fail(EINVAL);
    } else {
        int v = 0;
        if (level == SOL_SOCKET && name == SO_ERROR) {
            v = S[i].so_error;
            S[i].so_error = 0;
        } else if (level == SOL_SOCKET && name == SO_TYPE) {
            v = SOCK_STREAM;
        } else if (level == SOL_SOCKET && (name == SO_SNDBUF || name == SO_RCVBUF)) {
            v = RX_CAP;
        }
        memcpy(value, &v, sizeof v);
        *len = sizeof v;
    }
    pthread_mutex_unlock(&mu);
    return r;
}

int setsockopt(int fd, int level, int name, const void *value, socklen_t len)
{
    (void)level;
    (void)name;
    (void)value;
    (void)len;
    return netshim_is_shim_fd(fd) ? 0 : fail(in_range(fd) ? EBADF : ENOTSOCK);
}

ssize_t recvfrom(int fd, void *restrict buf, size_t len, int flags, struct sockaddr *restrict addr,
                 socklen_t *restrict alen)
{
    pthread_mutex_lock(&mu);
    const int i = idx(fd);
    ssize_t r;
    if (i < 0) {
        r = fail(in_range(fd) ? EBADF : ENOTSOCK);
    } else {
        r = do_recv(i, buf, len, flags);
        if (r >= 0 && addr && alen)
            fill_inet(addr, alen, S[i].rport);
    }
    pthread_mutex_unlock(&mu);
    return r;
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
    return recvfrom(fd, buf, len, flags, NULL, NULL);
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *addr, socklen_t alen)
{
    (void)addr;
    (void)alen;
    pthread_mutex_lock(&mu);
    const int i = idx(fd);
    const ssize_t r = i < 0 ? fail(in_range(fd) ? EBADF : ENOTSOCK) : do_send(i, buf, len, flags);
    pthread_mutex_unlock(&mu);
    return r;
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
    return sendto(fd, buf, len, flags, NULL, 0);
}

// --- fd API: shim fds here, everything else to Emscripten ------------------------

ssize_t read(int fd, void *buf, size_t count)
{
    if (in_range(fd))
        return recv(fd, buf, count, 0);
    __wasi_iovec_t iov = {.buf = buf, .buf_len = count};
    size_t n;
    return wasi_ret(__wasi_fd_read(fd, &iov, 1, &n)) ? -1 : (ssize_t)n;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    if (in_range(fd))
        return send(fd, buf, count, 0);
    __wasi_ciovec_t iov = {.buf = buf, .buf_len = count};
    size_t n;
    return wasi_ret(__wasi_fd_write(fd, &iov, 1, &n)) ? -1 : (ssize_t)n;
}

ssize_t readv(int fd, const struct iovec *iov, int count)
{
    if (!in_range(fd)) {
        size_t n;
        return wasi_ret(__wasi_fd_read(fd, (const __wasi_iovec_t *)iov, (size_t)count, &n)) ? -1 : (ssize_t)n;
    }
    ssize_t got = 0;
    for (int k = 0; k < count; k++) {
        const ssize_t r = recv(fd, iov[k].iov_base, iov[k].iov_len, got ? MSG_DONTWAIT : 0);
        if (r < 0)
            return got ? got : -1;
        got += r;
        if ((size_t)r < iov[k].iov_len)
            break;
    }
    return got;
}

ssize_t writev(int fd, const struct iovec *iov, int count)
{
    if (!in_range(fd)) {
        size_t n;
        return wasi_ret(__wasi_fd_write(fd, (const __wasi_ciovec_t *)iov, (size_t)count, &n)) ? -1 : (ssize_t)n;
    }
    ssize_t put = 0;
    for (int k = 0; k < count; k++) {
        const ssize_t r = send(fd, iov[k].iov_base, iov[k].iov_len, put ? MSG_DONTWAIT : 0);
        if (r < 0)
            return put ? put : -1;
        put += r;
        if ((size_t)r < iov[k].iov_len)
            break;
    }
    return put;
}

int close(int fd)
{
    if (!in_range(fd)) {
        const __wasi_errno_t r = __wasi_fd_close(fd);
        return wasi_ret(r == __WASI_ERRNO_INTR ? 0 : r);
    }
    pthread_mutex_lock(&mu);
    const int i = idx(fd);
    if (i >= 0)
        do_close(i);
    pthread_mutex_unlock(&mu);
    return i < 0 ? fail(EBADF) : 0;
}

int fcntl(int fd, int cmd, ...)
{
    unsigned long arg = 0;
    if (cmd != F_GETFL && cmd != F_GETFD && cmd != F_GETOWN) {
        va_list ap;
        va_start(ap, cmd);
        arg = va_arg(ap, unsigned long);
        va_end(ap);
    }
    if (!in_range(fd))
        return sys_ret(__syscall_fcntl64(fd, cmd, arg));

    pthread_mutex_lock(&mu);
    int r = 0;
    const int i = idx(fd);
    if (i < 0)
        r = fail(EBADF);
    else if (cmd == F_GETFL)
        r = O_RDWR | (S[i].nonblock ? O_NONBLOCK : 0);
    else if (cmd == F_SETFL)
        S[i].nonblock = !!(arg & O_NONBLOCK);
    else if (cmd == F_GETFD || cmd == F_SETFD)
        r = 0; // one process: close-on-exec means nothing
    else
        r = fail(EINVAL);
    pthread_mutex_unlock(&mu);
    return r;
}

int ioctl(int fd, int req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (!in_range(fd))
        return sys_ret(__syscall_ioctl(fd, req, arg));

    pthread_mutex_lock(&mu);
    int r = 0;
    const int i = idx(fd);
    if (i < 0)
        r = fail(EBADF);
    else if (req == FIONBIO)
        S[i].nonblock = !!*(int *)arg;
    else if (req == FIONREAD)
        *(int *)arg = (int)S[i].rx.len;
    else
        r = fail(ENOTTY);
    pthread_mutex_unlock(&mu);
    return r;
}

// --- poll / select -------------------------------------------------------------
//
// Shim fds wait on the shim's condition variable. If a call also has real
// fds (rare: Emscripten files), those are polled with timeout 0 and the wait
// is cut into 10 ms slices.

int poll(struct pollfd *fds, nfds_t n, int timeout)
{
    int has_shim = 0, has_real = 0;
    for (nfds_t k = 0; k < n; k++) {
        if (fds[k].fd < 0)
            continue;
        if (in_range(fds[k].fd))
            has_shim = 1;
        else
            has_real = 1;
    }
    if (!has_shim)
        return sys_ret(__syscall_poll((intptr_t)fds, (int)n, timeout));

    struct timespec deadline = {0, 0};
    if (timeout >= 0)
        deadline_in(&deadline, timeout);

    for (;;) {
        int ready = 0;
        pthread_mutex_lock(&mu);
        const unsigned seen = gen;
        for (nfds_t k = 0; k < n; k++) {
            if (fds[k].fd < 0 || !in_range(fds[k].fd))
                continue;
            const int i = idx(fds[k].fd);
            fds[k].revents = i < 0 ? POLLNVAL : revents_of(i, fds[k].events);
            if (fds[k].revents)
                ready++;
        }
        pthread_mutex_unlock(&mu);

        if (has_real) {
            for (nfds_t k = 0; k < n; k++) {
                if (fds[k].fd < 0 || in_range(fds[k].fd))
                    continue;
                struct pollfd one = fds[k];
                one.revents = 0;
                if (__syscall_poll((intptr_t)&one, 1, 0) > 0 && one.revents)
                    ready++;
                fds[k].revents = one.revents;
            }
        }
        if (ready || timeout == 0)
            return ready;

        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (timeout > 0 && (now.tv_sec > deadline.tv_sec ||
                            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)))
            return 0;

        struct timespec until = deadline;
        if (has_real) {
            deadline_in(&until, 10);
            if (timeout > 0 && (until.tv_sec > deadline.tv_sec ||
                                (until.tv_sec == deadline.tv_sec && until.tv_nsec > deadline.tv_nsec)))
                until = deadline;
        }
        pthread_mutex_lock(&mu);
        if (gen == seen)
            wait_change(timeout < 0 && !has_real ? NULL : &until);
        pthread_mutex_unlock(&mu);
    }
}

int select(int n, fd_set *restrict rfds, fd_set *restrict wfds, fd_set *restrict efds,
           struct timeval *restrict tv)
{
    struct pollfd pfds[FD_SETSIZE];
    nfds_t count = 0;
    for (int fd = 0; fd < n && fd < FD_SETSIZE; fd++) {
        short events = 0;
        if (rfds && FD_ISSET(fd, rfds))
            events |= POLLIN;
        if (wfds && FD_ISSET(fd, wfds))
            events |= POLLOUT;
        if (efds && FD_ISSET(fd, efds))
            events |= POLLPRI;
        if (events) {
            pfds[count].fd = fd;
            pfds[count].events = events;
            pfds[count].revents = 0;
            count++;
        }
    }
    const int timeout = tv ? (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;
    const int r = poll(pfds, count, timeout);
    if (r < 0)
        return r;
    if (rfds)
        FD_ZERO(rfds);
    if (wfds)
        FD_ZERO(wfds);
    if (efds)
        FD_ZERO(efds);
    int bits = 0;
    for (nfds_t k = 0; k < count; k++) {
        const short re = pfds[k].revents;
        if (rfds && (re & (POLLIN | POLLHUP | POLLERR)) && (pfds[k].events & POLLIN)) {
            FD_SET(pfds[k].fd, rfds);
            bits++;
        }
        if (wfds && (re & (POLLOUT | POLLERR)) && (pfds[k].events & POLLOUT)) {
            FD_SET(pfds[k].fd, wfds);
            bits++;
        }
        if (efds && (re & POLLPRI)) {
            FD_SET(pfds[k].fd, efds);
            bits++;
        }
    }
    return bits;
}
