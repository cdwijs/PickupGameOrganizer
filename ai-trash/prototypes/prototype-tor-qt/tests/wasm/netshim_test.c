// Unit test for src/wasm/netshim.c, built with Emscripten + pthreads and run
// under Node: tests/wasm/run-tests.sh

#include "netshim.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #cond, errno);      \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static struct sockaddr_in loopback(uint16_t port)
{
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = htonl(0x7f000001);
    return sin;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void test_socketpair(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(netshim_is_shim_fd(sv[0]) && netshim_is_shim_fd(sv[1]));
    CHECK(write(sv[0], "hello", 5) == 5);
    char buf[16] = {0};
    CHECK(read(sv[1], buf, sizeof buf) == 5 && memcmp(buf, "hello", 5) == 0);

    // Non-blocking read on an empty socket.
    CHECK(fcntl(sv[1], F_SETFL, O_NONBLOCK) == 0);
    CHECK((fcntl(sv[1], F_GETFL) & O_NONBLOCK) != 0);
    CHECK(read(sv[1], buf, sizeof buf) == -1 && errno == EAGAIN);

    // writev / readv across iovecs.
    struct iovec out[2] = {{"ab", 2}, {"cde", 3}};
    CHECK(writev(sv[1], out, 2) == 5);
    char a[2], b[8];
    struct iovec in[2] = {{a, 2}, {b, sizeof b}};
    CHECK(readv(sv[0], in, 2) == 5 && memcmp(a, "ab", 2) == 0 && memcmp(b, "cde", 3) == 0);

    // Closing one end: EOF for the other, and EPIPE on write.
    CHECK(close(sv[0]) == 0);
    CHECK(read(sv[1], buf, sizeof buf) == 0);
    CHECK(write(sv[1], "x", 1) == -1 && errno == EPIPE);
    struct pollfd p = {sv[1], POLLIN | POLLOUT, 0};
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLHUP));
    CHECK(close(sv[1]) == 0);
    CHECK(close(sv[1]) == -1 && errno == EBADF);
}

static void test_pipe(void)
{
    int fd[2];
    CHECK(pipe2(fd, O_NONBLOCK) == 0);
    CHECK(write(fd[1], "z", 1) == 1);
    struct pollfd p = {fd[0], POLLIN, 0};
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLIN));
    char c;
    CHECK(read(fd[0], &c, 1) == 1 && c == 'z');
    CHECK(read(fd[0], &c, 1) == -1 && errno == EAGAIN);
    CHECK(close(fd[0]) == 0 && close(fd[1]) == 0);
}

static void test_no_network(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s >= NETSHIM_FD_BASE);
    struct sockaddr_in guard = loopback(443);
    guard.sin_addr.s_addr = htonl(0x5db8d8f1); // some public address
    CHECK(connect(s, (struct sockaddr *)&guard, sizeof guard) == -1 && errno == ENETUNREACH);
    struct sockaddr_in nobody = loopback(1);
    CHECK(connect(s, (struct sockaddr *)&nobody, sizeof nobody) == -1 && errno == ECONNREFUSED);
    CHECK(socket(AF_INET6, SOCK_STREAM, 0) == -1 && errno == EAFNOSUPPORT);
    CHECK(socket(AF_INET, SOCK_DGRAM, 0) == -1 && errno == EPROTONOSUPPORT);
    close(s);
}

struct server_args {
    int listener;
    char got[64];
};

static void *server_thread(void *p)
{
    struct server_args *a = p;
    int c = accept(a->listener, NULL, NULL); // blocking accept
    if (c < 0)
        return NULL;
    ssize_t n = 0, r;
    while ((r = read(c, a->got + n, sizeof a->got - 1 - (size_t)n)) > 0) // blocking read until EOF
        n += r;
    write(c, "bye", 3);
    close(c);
    return NULL;
}

static void test_listen_connect_threads(void)
{
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in any = loopback(0);
    CHECK(bind(l, (struct sockaddr *)&any, sizeof any) == 0);
    CHECK(listen(l, 8) == 0);
    struct sockaddr_in bound;
    socklen_t len = sizeof bound;
    CHECK(getsockname(l, (struct sockaddr *)&bound, &len) == 0 && ntohs(bound.sin_port) >= 40000);

    // Port is taken now.
    int l2 = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(bind(l2, (struct sockaddr *)&bound, sizeof bound) == -1 && errno == EADDRINUSE);
    close(l2);

    struct server_args args = {.listener = l};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, server_thread, &args) == 0);

    int c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    CHECK(connect(c, (struct sockaddr *)&bound, sizeof bound) == 0);
    int err = -1;
    len = sizeof err;
    CHECK(getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0);
    struct sockaddr_in peer;
    len = sizeof peer;
    CHECK(getpeername(c, (struct sockaddr *)&peer, &len) == 0 && peer.sin_port == bound.sin_port);
    CHECK(send(c, "ping", 4, 0) == 4);
    CHECK(shutdown(c, SHUT_WR) == 0);

    // poll() must wake up when the other thread writes, not at the timeout.
    struct pollfd p = {c, POLLIN, 0};
    const long t0 = now_ms();
    CHECK(poll(&p, 1, 5000) == 1 && (p.revents & POLLIN));
    CHECK(now_ms() - t0 < 2000);
    char buf[8] = {0};
    CHECK(recv(c, buf, sizeof buf, 0) == 3 && memcmp(buf, "bye", 3) == 0);
    pthread_join(t, NULL);
    CHECK(strcmp(args.got, "ping") == 0);

    // poll() timeout with nothing to read.
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    struct pollfd q = {sv[0], POLLIN, 0};
    const long t1 = now_ms();
    CHECK(poll(&q, 1, 100) == 0);
    CHECK(now_ms() - t1 >= 90);
    close(sv[0]);
    close(sv[1]);
    close(c);
    close(l);
}

static void test_backpressure(void)
{
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
    static char chunk[64 * 1024];
    size_t total = 0;
    ssize_t r;
    while ((r = write(sv[0], chunk, sizeof chunk)) > 0)
        total += (size_t)r;
    CHECK(r == -1 && errno == EAGAIN);
    CHECK(total == 256 * 1024);
    struct pollfd p = {sv[0], POLLOUT, 0};
    CHECK(poll(&p, 1, 0) == 0);
    CHECK(read(sv[1], chunk, sizeof chunk) == (ssize_t)sizeof chunk);
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLOUT));
    close(sv[0]);
    close(sv[1]);
}

static int notified;
static void on_notify(int fd, void *ctx)
{
    (void)fd;
    (void)ctx;
    notified++;
}

static void test_notify(void)
{
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    CHECK(netshim_set_notify(sv[1], on_notify, NULL) == 0);
    write(sv[0], "x", 1);
    CHECK(notified == 1);
    close(sv[0]);
    CHECK(notified == 2);
    CHECK(netshim_set_notify(12345, on_notify, NULL) == -1 && errno == EBADF);
    close(sv[1]);
}

static void test_passthrough(void)
{
    // Real files still work through the overridden read/write/close/fcntl.
    int fd = open("/tmp/netshim-test.txt", O_CREAT | O_RDWR | O_TRUNC, 0600);
    CHECK(fd >= 0 && fd < NETSHIM_FD_BASE);
    CHECK(write(fd, "file", 4) == 4);
    CHECK(fcntl(fd, F_GETFL) >= 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    char buf[8] = {0};
    CHECK(read(fd, buf, sizeof buf) == 4 && memcmp(buf, "file", 4) == 0);
    CHECK(close(fd) == 0);
}

int main(void)
{
    test_socketpair();
    test_pipe();
    test_no_network();
    test_listen_connect_threads();
    test_backpressure();
    test_notify();
    test_passthrough();
    printf(failures ? "netshim_test: %d FAILED\n" : "netshim_test: all passed\n", failures);
    return failures ? 1 : 0;
}
