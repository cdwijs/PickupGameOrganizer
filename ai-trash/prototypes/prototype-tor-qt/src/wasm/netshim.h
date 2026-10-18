#pragma once

// netshim: an in-process replacement for the libc socket API, for the
// WebAssembly build only.
//
// A browser tab cannot open TCP sockets, but tor, the app and the Snowflake
// client still talk to each other over sockets: the owning control
// socketpair, tor's SOCKS port, the PT SOCKS port and the onion service
// target. netshim overrides socket(), socketpair(), pipe(), bind(), listen(),
// accept(), connect(), send()/recv(), read()/write()/close(), fcntl(), poll()
// and select() so that all of these become in-memory byte pipes inside the
// tab. Only 127.0.0.1 exists: connect() to anything else fails with
// ENETUNREACH, so tor can leave the tab only through the Snowflake PT.
//
// Shim fds are numbered from NETSHIM_FD_BASE; every other fd (files, stdout)
// is passed through to Emscripten unchanged. All state is protected by one
// mutex, so the calls are safe from tor's pthread and the Qt (browser main)
// thread alike. The main thread must use non-blocking sockets: it may not
// block in a browser.

#ifdef __cplusplus
extern "C" {
#endif

#define NETSHIM_FD_BASE 512
#define NETSHIM_MAX_FDS 480 // stays below FD_SETSIZE (1024) for select()

// Called whenever the readiness of a shim fd may have changed (data arrived,
// buffer drained, peer closed, connection queued). It is called with
// netshim's lock held, from whichever thread caused the change, so it must
// only hand off (e.g. post to an event loop) and never call back into netshim.
typedef void (*netshim_notify_fn)(int fd, void *ctx);

// Register (or clear, with fn == 0) the readiness callback for a shim fd.
// Returns -1 with errno EBADF if fd is not an open shim fd.
int netshim_set_notify(int fd, netshim_notify_fn fn, void *ctx);

// 1 if fd is an open shim fd.
int netshim_is_shim_fd(int fd);

#ifdef __cplusplus
}
#endif
