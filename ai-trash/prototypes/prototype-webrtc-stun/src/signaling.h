#pragma once

// Signaling over public ntfy servers (resources/ntfy-servers.txt, in order).
// A node listens on its own topic on *every* server; a message is posted to
// one server at a time, chosen by the caller.
//
// Subscriptions: natively a streaming GET of https://<server>/<topic>/json,
// in the browser a WebSocket to wss://<server>/<topic>/ws (Qt for wasm only
// delivers a reply once it is complete). Both produce the same JSON events.

#include <QObject>
#include <QStringList>

#include <functional>
#include <vector>

class QNetworkAccessManager;
class Subscription;

class Signaling : public QObject
{
    Q_OBJECT

public:
    explicit Signaling(QObject *parent = nullptr);
    ~Signaling() override;

    const QStringList &servers() const { return m_servers; }

    // Drops the old subscriptions and listens on `topic` on every server.
    void listen(const QString &topic);

    // POSTs `body` to https://<server>/<topic>. `done` gets an empty string
    // on success, otherwise the error.
    void publish(int server, const QString &topic, const QByteArray &body,
                 std::function<void(const QString &error)> done);

signals:
    void message(int server, const QByteArray &body);
    void log(const QString &line);

private:
    QNetworkAccessManager *m_nam;
    QStringList m_servers;
    std::vector<Subscription *> m_subscriptions;
};
