#pragma once

// One message channel that carries a Session's bytes (turbotunnel token +
// client ID, then encapsulated KCP packets) towards the Snowflake bridge:
// a WebRTC data channel to a volunteer proxy (WebRtcPeer) or, in the browser,
// a WebSocket straight to the bridge (WebSocketPeer). The bridge cannot tell
// the two apart, so a Session can switch between them and keep its KCP state.

#include <QByteArray>
#include <QObject>
#include <QString>

namespace snowflake {

class Peer : public QObject
{
    Q_OBJECT

public:
    virtual bool send(const QByteArray &bytes) = 0;
    bool isOpen() const { return m_open; }
    const QString &label() const { return m_label; }

signals:
    void opened();
    void message(const QByteArray &bytes);
    void closed(const QString &reason);
    void log(const QString &line);

protected:
    Peer(const QString &label, QObject *parent) : QObject(parent), m_label(label) {}

    void markOpen()
    {
        if (m_closed)
            return;
        m_open = true;
        emit opened();
    }

    void fail(const QString &reason)
    {
        if (m_closed)
            return;
        m_closed = true;
        m_open = false;
        emit closed(reason);
    }

    bool isClosed() const { return m_closed; }

private:
    QString m_label;
    bool m_open = false;
    bool m_closed = false;
};

} // namespace snowflake
