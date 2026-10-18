#include "client.h"

#include <QHostAddress>

#include <rtc/global.hpp>

namespace snowflake {

#ifdef Q_OS_WASM
namespace {
// WebSocket URLs of the default bridges (broker bridge list, 2026-10), keyed by
// fingerprint: where a volunteer proxy would connect on our behalf.
struct Relay
{
    const char *fingerprint;
    const char *url;
};
constexpr Relay kRelays[] = {
    {"2B280B23E1107BB62ABFC40DDCC8824814F80A72", "wss://snowflake.torproject.net/"},
    {"8838024498816A039FCBBAB14E6F40A0843051FA", "wss://02.snowflake.torproject.net/"},
};
} // namespace
#endif

Client::Client(QObject *parent) : QObject(parent)
{
    // libdatachannel/libjuice warnings (ICE, DTLS, SCTP) into the debug log.
    rtc::InitLogger(rtc::LogLevel::Warning, [this](rtc::LogLevel, std::string message) {
        QMetaObject::invokeMethod(this, [this, m = QString::fromStdString(message)] {
            emit log(QStringLiteral("[webrtc] ") + m);
        }, Qt::QueuedConnection);
    });
    connect(&m_server, &LoopbackServer::newConnection, this, [this] {
        while (LoopbackSocket *socket = m_server.nextPendingConnection()) {
            m_handshakes.insert(socket, {});
            connect(socket, &LoopbackSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
            connect(socket, &LoopbackSocket::disconnected, this, [this, socket] {
                if (m_handshakes.remove(socket))
                    socket->deleteLater();
            });
        }
    });
}

Client::~Client()
{
    rtc::InitLogger(rtc::LogLevel::None); // the callback above captures `this`
}

quint16 Client::listen()
{
    if (!m_server.isListening())
        m_server.listen(QHostAddress::LocalHost, 0);
    return m_server.serverPort();
}

void Client::onReadyRead(LoopbackSocket *socket)
{
    auto it = m_handshakes.find(socket);
    if (it == m_handshakes.end())
        return; // already handed to a Session
    it->buffer += socket->readAll();
    while (m_handshakes.contains(socket) && step(socket, *it)) {
    }
}

bool Client::step(LoopbackSocket *socket, Handshake &hs)
{
    QByteArray &b = hs.buffer;
    auto fail = [&](const QString &why) {
        emit log(QStringLiteral("[snowflake] SOCKS handshake failed: ") + why);
        m_handshakes.remove(socket);
        socket->abort();
        socket->deleteLater();
        return false;
    };

    switch (hs.state) {
    case State::Greeting: { // VER NMETHODS METHODS...
        if (b.size() < 2)
            return false;
        const int n = quint8(b[1]);
        if (b.size() < 2 + n)
            return false;
        if (b[0] != 5)
            return fail(QStringLiteral("not SOCKS5"));
        const QByteArray methods = b.mid(2, n);
        b.remove(0, 2 + n);
        if (methods.contains(char(0x02))) { // username/password carries the PT args
            socket->write(QByteArray("\x05\x02", 2));
            hs.state = State::Auth;
        } else if (methods.contains(char(0x00))) {
            socket->write(QByteArray("\x05\x00", 2));
            hs.state = State::Request;
        } else {
            socket->write(QByteArray("\x05\xff", 2));
            return fail(QStringLiteral("no acceptable auth method"));
        }
        return true;
    }
    case State::Auth: { // VER ULEN UNAME PLEN PASSWD
        if (b.size() < 2)
            return false;
        const int ulen = quint8(b[1]);
        if (b.size() < 3 + ulen)
            return false;
        const int plen = quint8(b[2 + ulen]);
        if (b.size() < 3 + ulen + plen)
            return false;
        hs.args = b.mid(2, ulen) + b.mid(3 + ulen, plen);
        // tor pads an empty password with a single NUL byte.
        while (hs.args.endsWith('\0'))
            hs.args.chop(1);
        b.remove(0, 3 + ulen + plen);
        socket->write(QByteArray("\x01\x00", 2));
        hs.state = State::Request;
        return true;
    }
    case State::Request: { // VER CMD RSV ATYP DST.ADDR DST.PORT
        if (b.size() < 5)
            return false;
        int addrLen = 0;
        switch (quint8(b[3])) {
        case 1: addrLen = 4; break;
        case 4: addrLen = 16; break;
        case 3: addrLen = 1 + quint8(b[4]); break;
        default: return fail(QStringLiteral("bad address type"));
        }
        if (b.size() < 4 + addrLen + 2)
            return false;
        if (b[1] != 1)
            return fail(QStringLiteral("only CONNECT is supported"));
        b.remove(0, 4 + addrLen + 2);
        // The bridge address (192.0.2.3:80) is a placeholder; Snowflake picks the route.
        socket->write(QByteArray("\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10));
        const QByteArray args = hs.args;
        const QByteArray early = b; // tor may pipeline its TLS ClientHello
        m_handshakes.remove(socket);
        disconnect(socket, nullptr, this, nullptr);
        startSession(socket, args);
        if (!early.isEmpty())
            emit log(QStringLiteral("[snowflake] warning: %1 early bytes dropped").arg(early.size()));
        return false;
    }
    }
    return false;
}

void Client::checkPeers()
{
    for (Session *session : findChildren<Session *>(Qt::FindDirectChildrenOnly))
        session->checkPeer();
}

void Client::startSession(LoopbackSocket *socket, const QByteArray &args)
{
    QStringList warnings;
    const BridgeArgs bridge = parseBridgeArgs(args, &warnings);
    for (const QString &w : warnings)
        emit log(QStringLiteral("[snowflake] ") + w);
    if (!bridge.brokerUrl.isValid() || bridge.brokerUrl.isEmpty()) {
        emit log(QStringLiteral("[snowflake] bridge line has no url= broker; closing"));
        socket->abort();
        socket->deleteLater();
        return;
    }
    auto *session = new Session(bridge, socket, ++m_sessionCounter, this);
    connect(session, &Session::log, this, &Client::log);
}

BridgeArgs parseBridgeArgs(const QByteArray &args, QStringList *warnings)
{
    QHash<QString, QString> kv;
    QString key, value, *current = &key;
    auto flush = [&] {
        if (!key.isEmpty())
            kv.insert(key, value);
        key.clear();
        value.clear();
        current = &key;
    };
    const QString text = QString::fromUtf8(args);
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\\') && i + 1 < text.size()) {
            *current += text.at(++i);
        } else if (c == QLatin1Char('=') && current == &key) {
            current = &value;
        } else if (c == QLatin1Char(';')) {
            flush();
        } else {
            *current += c;
        }
    }
    flush();

    BridgeArgs bridge;
    bridge.brokerUrl = QUrl(kv.value(QStringLiteral("url")));
    QString fronts = kv.value(QStringLiteral("fronts"));
    if (fronts.isEmpty())
        fronts = kv.value(QStringLiteral("front"));
    if (!fronts.isEmpty())
        bridge.fronts = fronts.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &ice : kv.value(QStringLiteral("ice")).split(QLatin1Char(','), Qt::SkipEmptyParts))
        bridge.iceServers << ice.trimmed();
    bridge.fingerprint = kv.value(QStringLiteral("fingerprint"));

    for (const QString &unsupported : {QStringLiteral("ampcache"), QStringLiteral("sqsqueue")}) {
        if (kv.contains(unsupported))
            *warnings << QStringLiteral("%1= is not supported, using url= domain fronting").arg(unsupported);
    }
    if (kv.contains(QStringLiteral("utls-imitate")))
        *warnings << QStringLiteral("utls-imitate is ignored; the broker request uses Qt's TLS stack");

#ifdef Q_OS_WASM
    // A page cannot choose the Host header separately from the URL.
    if (!bridge.fronts.isEmpty()) {
        *warnings << QStringLiteral("domain fronting is impossible in a browser; asking %1 directly")
                         .arg(bridge.brokerUrl.host());
        bridge.fronts.clear();
    }
    for (const Relay &relay : kRelays) {
        if (bridge.fingerprint.compare(QLatin1String(relay.fingerprint), Qt::CaseInsensitive) == 0)
            bridge.relayUrl = QUrl(QString::fromLatin1(relay.url));
    }
    if (!bridge.relayUrl.isValid())
        *warnings << QStringLiteral("no WebSocket URL known for bridge %1; WebRTC proxies only").arg(bridge.fingerprint);
#endif
    return bridge;
}

} // namespace snowflake
