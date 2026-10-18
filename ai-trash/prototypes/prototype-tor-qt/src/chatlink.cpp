#include "chatlink.h"

#include <QHostAddress>
#include <QNetworkProxy>
#include <QtEndian>

namespace {
constexpr quint32 kMaxFrame = 64 * 1024;

// Accept "abc...xyz", "abc...xyz.onion" or "/onion3/abc...xyz:9999".
QString normalizeOnion(QString peer)
{
    peer = peer.trimmed();
    if (peer.startsWith(QLatin1String("/onion3/")))
        peer = peer.mid(8).section(QLatin1Char(':'), 0, 0);
    if (!peer.endsWith(QLatin1String(".onion")))
        peer += QLatin1String(".onion");
    return peer.toLower();
}
} // namespace

ChatLink::ChatLink(QObject *parent) : QObject(parent)
{
    connect(&m_server, &LoopbackServer::newConnection, this, [this] {
        while (LoopbackSocket *socket = m_server.nextPendingConnection())
            adopt(socket, QStringLiteral("incoming peer"));
    });
}

quint16 ChatLink::listen()
{
    if (!m_server.isListening())
        m_server.listen(QHostAddress::LocalHost, 0);
    return m_server.serverPort();
}

void ChatLink::send(const QString &peer, const QString &text)
{
    const QString target = peer.trimmed().isEmpty() ? QString() : normalizeOnion(peer);

    // Reuse the current connection unless the user typed a different peer.
    const bool usable = m_active && m_active->state() == QAbstractSocket::ConnectedState;
    if (usable && (target.isEmpty() || target == m_activePeer || m_activePeer.isEmpty())) {
        writeFrame(m_active, text);
        return;
    }
    if (target.isEmpty()) {
        emit log(QStringLiteral("No connection: enter the peer's onion address first"));
        return;
    }
    m_queued << text;
    const bool dialling = m_active && m_activePeer == target
                          && m_active->state() != QAbstractSocket::UnconnectedState;
    if (!dialling)
        dial(target);
}

void ChatLink::dial(const QString &onion)
{
    if (m_socksPort == 0) {
        emit log(QStringLiteral("Tor SOCKS port not ready yet"));
        m_queued.clear();
        return;
    }
    if (m_active)
        m_active->disconnectFromHost();

    auto *socket = new LoopbackSocket(this);
    // Socks5 in Qt resolves hostnames on the proxy side, which is what .onion needs.
    socket->setProxy(QNetworkProxy(QNetworkProxy::Socks5Proxy, QStringLiteral("127.0.0.1"), m_socksPort));
    adopt(socket, onion);
    m_activePeer = onion;

    connect(socket, &LoopbackSocket::connected, this, [this, socket, onion] {
        emit log(QStringLiteral("Connected to %1").arg(onion));
        for (const QString &text : std::exchange(m_queued, {}))
            writeFrame(socket, text);
    });
    emit log(QStringLiteral("Dialling %1:%2 via Tor (can take 10-60 s)").arg(onion).arg(kVirtualPort));
    socket->connectToHost(onion, kVirtualPort);
}

void ChatLink::adopt(LoopbackSocket *socket, const QString &label)
{
    if (socket->state() == QAbstractSocket::ConnectedState) {
        emit log(QStringLiteral("Accepted %1").arg(label));
        // Replies to an incoming peer go back over its own connection.
        m_activePeer.clear();
    }
    m_active = socket;

    connect(socket, &LoopbackSocket::readyRead, this, [this, socket, label] { readFrames(socket, label); });
    connect(socket, &LoopbackSocket::errorOccurred, this, [this, socket, label](QAbstractSocket::SocketError) {
        emit log(QStringLiteral("%1: %2").arg(label, socket->errorString()));
        // A dial that never connected emits no disconnected(); clean up here.
        if (socket->state() == QAbstractSocket::UnconnectedState) {
            if (socket == m_active)
                m_queued.clear();
            m_buffers.remove(socket);
            socket->deleteLater();
        }
    });
    connect(socket, &LoopbackSocket::disconnected, this, [this, socket, label] {
        emit log(QStringLiteral("Disconnected from %1").arg(label));
        m_buffers.remove(socket);
        socket->deleteLater();
    });
}

void ChatLink::readFrames(LoopbackSocket *socket, const QString &label)
{
    QByteArray &buffer = m_buffers[socket];
    buffer += socket->readAll();
    while (buffer.size() >= 4) {
        const quint32 len = qFromLittleEndian<quint32>(buffer.constData());
        if (len > kMaxFrame) {
            emit log(QStringLiteral("Frame too large (%1 bytes), closing").arg(len));
            socket->abort();
            return;
        }
        if (quint32(buffer.size()) < 4 + len)
            return;
        const QString text = QString::fromUtf8(buffer.mid(4, len));
        buffer.remove(0, 4 + len);
        emit received(label, text);
    }
}

void ChatLink::writeFrame(LoopbackSocket *socket, const QString &text)
{
    const QByteArray payload = text.toUtf8();
    if (quint32(payload.size()) > kMaxFrame) {
        emit log(QStringLiteral("Message too long"));
        return;
    }
    char len[4];
    qToLittleEndian<quint32>(quint32(payload.size()), len);
    socket->write(len, 4);
    socket->write(payload);
    emit log(QStringLiteral("Sent %1 bytes").arg(payload.size()));
}
