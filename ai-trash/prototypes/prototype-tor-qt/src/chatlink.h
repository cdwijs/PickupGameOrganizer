#pragma once

// One chat connection over Tor. Incoming peers reach the local server through
// our onion service; outgoing connections go through tor's SOCKS port.
// Wire format matches prototype-arti: u32 little-endian length + UTF-8 text.

#include "loopback.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>

class ChatLink : public QObject
{
    Q_OBJECT

public:
    static constexpr quint16 kVirtualPort = 9999;

    explicit ChatLink(QObject *parent = nullptr);

    // Listen on 127.0.0.1:<random>; the onion service forwards here.
    quint16 listen();

    void setSocksPort(quint16 port) { m_socksPort = port; }

    // Send over the active connection, dialling `peer` first if needed.
    void send(const QString &peer, const QString &text);

signals:
    void received(const QString &from, const QString &text);
    void log(const QString &line);

private:
    void adopt(LoopbackSocket *socket, const QString &label);
    void dial(const QString &peer);
    void readFrames(LoopbackSocket *socket, const QString &label);
    void writeFrame(LoopbackSocket *socket, const QString &text);

    LoopbackServer m_server;
    QPointer<LoopbackSocket> m_active;
    QString m_activePeer;
    QStringList m_queued; // messages waiting for an outgoing dial to finish
    QHash<LoopbackSocket *, QByteArray> m_buffers;
    quint16 m_socksPort = 0;
};
