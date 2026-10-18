#pragma once

// In-process Snowflake pluggable-transport client. Tor talks to it as an
// external PT proxy (torrc: ClientTransportPlugin snowflake socks5 127.0.0.1:<port>):
// one SOCKS5 connection per bridge, with the bridge line's key=value arguments
// in the SOCKS5 username/password fields (pt-spec "Passing PT-specific parameters").

#include "loopback.h"
#include "session.h"

#include <QHash>
#include <QObject>

namespace snowflake {

class Client : public QObject
{
    Q_OBJECT

public:
    explicit Client(QObject *parent = nullptr);
    ~Client() override;

    quint16 listen(); // 127.0.0.1:<random>
    void checkPeers(); // after the app was in the background

signals:
    void log(const QString &line);

private:
    enum class State { Greeting, Auth, Request };
    struct Handshake
    {
        State state = State::Greeting;
        QByteArray buffer;
        QByteArray args;
    };

    void onReadyRead(LoopbackSocket *socket);
    bool step(LoopbackSocket *socket, Handshake &hs); // false: need more bytes
    void startSession(LoopbackSocket *socket, const QByteArray &args);

    LoopbackServer m_server;
    QHash<LoopbackSocket *, Handshake> m_handshakes;
    int m_sessionCounter = 0;
};

// Parse "k=v;k=v" with \; \= \\ escapes into bridge settings. In the browser,
// fronts= is dropped and relayUrl is filled in from the fingerprint.
BridgeArgs parseBridgeArgs(const QByteArray &args, QStringList *warnings);

} // namespace snowflake
