// Spike: does the real tor (libtor.a) run under Emscripten + pthreads on top
// of netshim? Runs under Node, no network needed:
//   1. tor starts on its own pthread, with an owning control socketpair;
//   2. the control protocol answers (GETINFO version, net/listeners/socks);
//   3. tor's SOCKS port accepts a SOCKS5 handshake from this "app";
//   4. in Snowflake mode tor connects to the PT's SOCKS port (a listener this
//      test opens in place of snowflake::Client) and speaks SOCKS5 to it.
//
//   tests/wasm/run-tests.sh

#include "netshim.h"
#include "tor_api.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #cond, errno);      \
            failures++;                                                                            \
        } else {                                                                                   \
            printf("ok   %s\n", #cond);                                                            \
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

// Read until "\r\n<code> " ends a control reply, or timeout. Returns the reply.
static char reply[16384];
static const char *control(int fd, const char *cmd, int timeout_ms)
{
    if (cmd)
        write(fd, cmd, strlen(cmd));
    size_t n = 0;
    reply[0] = 0;
    for (;;) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, timeout_ms) <= 0)
            return reply;
        const ssize_t r = read(fd, reply + n, sizeof reply - 1 - n);
        if (r <= 0)
            return reply;
        n += (size_t)r;
        reply[n] = 0;
        // Last line of a reply: "DDD " at the start of a line, ending in CRLF.
        for (char *line = reply; line && *line; line = strstr(line, "\r\n") ? strstr(line, "\r\n") + 2 : NULL) {
            if (strlen(line) >= 4 && line[3] == ' ' && strstr(line, "\r\n") && strncmp(line, "650", 3) != 0)
                return reply;
        }
    }
}

static void *tor_thread(void *cfg)
{
    const int rc = tor_run_main(cfg);
    printf("tor_run_main returned %d\n", rc);
    return NULL;
}

int main(void)
{
    // In place of snowflake::Client: the PT SOCKS listener tor must reach.
    const int pt = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in any = loopback(0);
    bind(pt, (struct sockaddr *)&any, sizeof any);
    listen(pt, 8);
    struct sockaddr_in ptaddr;
    socklen_t len = sizeof ptaddr;
    getsockname(pt, (struct sockaddr *)&ptaddr, &len);
    const int ptport = ntohs(ptaddr.sin_port);

    mkdir("/tor", 0700);
    FILE *f = fopen("/tor/torrc", "w");
    fprintf(f,
            "DataDirectory /tor\n"
            "SocksPort auto\n"
            "ClientOnly 1\n"
            "AvoidDiskWrites 1\n"
            "Log notice stdout\n"
            "__DisableSignalHandlers 1\n"
            "UseBridges 1\n"
            "ClientTransportPlugin snowflake socks5 127.0.0.1:%d\n"
            "Bridge snowflake 192.0.2.3:80 2B280B23E1107BB62ABFC40DDCC8824814F80A72 "
            "fingerprint=2B280B23E1107BB62ABFC40DDCC8824814F80A72 url=https://snowflake-broker.torproject.net/\n",
            ptport);
    fclose(f);
    fclose(fopen("/tor/torrc-defaults", "w"));

    char *argv[] = {"tor", "-f", "/tor/torrc", "--defaults-torrc", "/tor/torrc-defaults", NULL};
    tor_main_configuration_t *cfg = tor_main_configuration_new();
    tor_main_configuration_set_command_line(cfg, 5, argv);
    const tor_control_socket_t ctl = tor_main_configuration_setup_control_socket(cfg);
    CHECK(ctl != INVALID_TOR_CONTROL_SOCKET && netshim_is_shim_fd(ctl));
    printf("provider: %s\n", tor_api_get_provider_version());

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    pthread_t t;
    CHECK(pthread_create(&t, &attr, tor_thread, cfg) == 0);

    // 2. The control protocol.
    const char *r = control(ctl, "GETINFO version\r\n", 30000);
    CHECK(strstr(r, "250-version=") != NULL);
    printf("     %.60s\n", r);
    r = control(ctl, "GETINFO net/listeners/socks\r\n", 10000);
    CHECK(strstr(r, "250-net/listeners/socks=\"127.0.0.1:") != NULL);
    int socksport = 0;
    const char *colon = strstr(r, "127.0.0.1:");
    if (colon)
        socksport = atoi(colon + 10);
    printf("     tor SOCKS port %d, PT port %d\n", socksport, ptport);

    // 3. SOCKS5 to tor, as ChatLink does.
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = loopback((uint16_t)socksport);
    CHECK(connect(s, (struct sockaddr *)&sa, sizeof sa) == 0);
    write(s, "\x05\x01\x00", 3);
    unsigned char hello[2] = {0};
    struct pollfd ps = {s, POLLIN, 0};
    CHECK(poll(&ps, 1, 10000) == 1 && read(s, hello, 2) == 2 && hello[0] == 5 && hello[1] == 0);
    close(s);

    // 4. tor reaches the PT. It does so once it wants the bridge, i.e. right
    // after start (it has no consensus yet).
    struct pollfd pp = {pt, POLLIN, 0};
    CHECK(poll(&pp, 1, 60000) == 1);
    const int c = accept(pt, NULL, NULL);
    CHECK(c >= 0);
    unsigned char greet[8] = {0};
    struct pollfd pc = {c, POLLIN, 0};
    CHECK(poll(&pc, 1, 10000) == 1 && read(c, greet, sizeof greet) >= 2 && greet[0] == 5);
    printf("     PT got SOCKS5 greeting with %d method(s)\n", greet[1]);
    close(c);

    // tor's bootstrap state, for the log.
    r = control(ctl, "GETINFO status/bootstrap-phase\r\n", 5000);
    printf("     %s", r);

    // Closing the owning control socket makes tor exit.
    close(ctl);
    pthread_join(t, NULL);
    printf(failures ? "tor_spike: %d FAILED\n" : "tor_spike: all passed\n", failures);
    return failures ? 1 : 0;
}
