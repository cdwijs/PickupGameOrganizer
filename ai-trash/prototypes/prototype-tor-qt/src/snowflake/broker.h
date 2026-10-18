#pragma once

// Client side of the Snowflake broker protocol (common/messages/client.go):
// POST <broker>/client with "1.0\n{offer, nat, fingerprint}" and read back
// {answer} or {error}. Domain fronting puts a front domain in the URL (and so
// in DNS and TLS SNI) and the real broker host in the HTTP Host header.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;

namespace snowflake {

class Broker : public QObject
{
    Q_OBJECT

public:
    Broker(const QUrl &brokerUrl, const QStringList &fronts, QObject *parent = nullptr);

    // offerSdp/answerSdp are plain SDP text; the JSON wrapping is done here.
    using Callback = std::function<void(const QString &answerSdp, const QString &error)>;
    void negotiate(const QString &offerSdp, const QString &natType, const QString &fingerprint,
                   Callback callback);

signals:
    void log(const QString &line);

private:
    QNetworkAccessManager *m_network;
    QUrl m_brokerUrl;
    QStringList m_fronts;
};

} // namespace snowflake
