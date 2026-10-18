#pragma once

// The app reaches its embedded tor over loopback sockets only: the owning
// control connection, tor's SOCKS port, the Snowflake PT port and the onion
// service target. Native builds use Qt's own sockets. In the browser all of
// these are netshim fds (src/wasm/netshim.h), which Qt's socket notifiers
// cannot see, so ShimSocket/ShimServer stand in with the same API subset.

#include <QtGlobal>

#ifdef Q_OS_WASM
#include "wasm/shimsocket.h"
using LoopbackSocket = ShimSocket;
using LoopbackServer = ShimServer;
using ControlSocket = ShimSocket;
#else
#include <QLocalSocket>
#include <QTcpServer>
#include <QTcpSocket>
using LoopbackSocket = QTcpSocket;
using LoopbackServer = QTcpServer;
using ControlSocket = QLocalSocket;
#endif
