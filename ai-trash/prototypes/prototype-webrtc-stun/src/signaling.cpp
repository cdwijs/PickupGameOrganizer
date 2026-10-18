#include "signaling.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>

#ifdef Q_OS_WASM
#include <rtc/rtc.hpp>
#endif

namespace {

// ntfy sends a keepalive event every 45 s by default; silence for longer than
// this means the stream is dead even if the connection looks open.
constexpr int kStallMs = 120000;
constexpr int kMaxBackoffMs = 60000;
constexpr int kPublishTimeoutMs = 10000;

QStringList loadServers()
{
    QStringList servers;
    QFile file(QStringLiteral(":/resources/ntfy-servers.txt"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (const QByteArray &line : file.readAll().split('\n')) {
            const QString server = QString::fromUtf8(line).trimmed();
            if (!server.isEmpty() && !server.startsWith(QLatin1Char('#')))
                servers << server;
        }
    }
    return servers;
}

} // namespace

// One subscription to one ntfy server, reconnecting with backoff.
class Subscription : public QObject
{
public:
    Subscription(QNetworkAccessManager *nam, int index, const QString &server, const QString &topic,
                 Signaling *owner)
        : QObject(owner), m_owner(owner), m_nam(nam), m_index(index), m_server(server), m_topic(topic)
    {
        m_retry.setSingleShot(true);
        connect(&m_retry, &QTimer::timeout, this, [this] { open(); });
        m_stall.setSingleShot(true);
        m_stall.setInterval(kStallMs);
        connect(&m_stall, &QTimer::timeout, this, [this] { reconnect(QStringLiteral("no data for 120 s")); });
        Q_UNUSED(m_nam);
        open();
    }

    ~Subscription() override { close(); }

private:
    void open()
    {
        close();
        m_clock.start();
#ifdef Q_OS_WASM
        // datachannel-wasm calls back on this (the only) thread; destroying
        // the socket detaches its JS handlers.
        m_ws = std::make_shared<rtc::WebSocket>();
        m_ws->onMessage([this](rtc::message_variant data) {
            if (const auto *text = std::get_if<std::string>(&data))
                handleEvent(QByteArray::fromStdString(*text));
        });
        m_ws->onClosed([this] {
            QMetaObject::invokeMethod(this, [this] { reconnect(QStringLiteral("WebSocket closed")); },
                                      Qt::QueuedConnection);
        });
        m_ws->onError([this](std::string error) {
            QMetaObject::invokeMethod(this, [this, e = QString::fromStdString(error)] {
                reconnect(QStringLiteral("WebSocket error: ") + e);
            }, Qt::QueuedConnection);
        });
        m_ws->open(QStringLiteral("wss://%1/%2/ws").arg(m_server, m_topic).toStdString());
#else
        QNetworkRequest request(QUrl(QStringLiteral("https://%1/%2/json").arg(m_server, m_topic)));
        request.setTransferTimeout(0); // a stream that never ends; m_stall watches it instead
        m_reply = m_nam->get(request);
        connect(m_reply, &QNetworkReply::readyRead, this, [this] {
            m_buffer += m_reply->readAll();
            qsizetype newline;
            while ((newline = m_buffer.indexOf('\n')) >= 0) {
                const QByteArray line = m_buffer.left(newline);
                m_buffer.remove(0, newline + 1);
                if (!line.trimmed().isEmpty())
                    handleEvent(line);
            }
        });
        connect(m_reply, &QNetworkReply::finished, this, [this] {
            const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QString why = QStringLiteral("stream ended");
            if (status && status != 200)
                why = QStringLiteral("HTTP %1").arg(status); // e.g. 429 when the server rate-limits us
            else if (m_reply->error() != QNetworkReply::NoError)
                why = m_reply->errorString();
            reconnect(why);
        });
#endif
        m_stall.start();
    }

    void close()
    {
        m_stall.stop();
#ifdef Q_OS_WASM
        if (m_ws) {
            m_ws->close();
            m_ws.reset();
        }
#else
        if (m_reply) {
            m_reply->disconnect(this);
            m_reply->abort();
            m_reply->deleteLater();
            m_reply = nullptr;
        }
        m_buffer.clear();
#endif
    }

    void reconnect(const QString &why)
    {
        close();
        if (m_retry.isActive())
            return;
        // 1 s, 2 s, 4 s ... 60 s, with 30 % jitter so 8 servers don't retry in lockstep.
        const int delay = m_backoffMs + int(QRandomGenerator::global()->bounded(m_backoffMs * 3 / 10 + 1));
        m_backoffMs = qMin(m_backoffMs * 2, kMaxBackoffMs);
        if (m_listening || m_failures++ == 0)
            emit m_owner->log(QStringLiteral("ntfy %1: %2, retrying in %3 s").arg(m_server, why).arg(delay / 1000.0, 0, 'f', 1));
        m_listening = false;
        m_retry.start(delay);
    }

    void handleEvent(const QByteArray &line)
    {
        m_stall.start();
        const QJsonObject event = QJsonDocument::fromJson(line).object();
        const QString type = event.value(QLatin1String("event")).toString();
        if (type == QLatin1String("open")) {
            m_listening = true;
            m_failures = 0;
            m_backoffMs = 1000;
            emit m_owner->log(QStringLiteral("ntfy %1: listening (%2 ms)").arg(m_server).arg(m_clock.elapsed()));
        } else if (type == QLatin1String("message")) {
            emit m_owner->message(m_index, event.value(QLatin1String("message")).toString().toUtf8());
        }
    }

    Signaling *m_owner;
    QNetworkAccessManager *m_nam;
    const int m_index;
    const QString m_server;
    const QString m_topic;
    QTimer m_retry;
    QTimer m_stall;
    QElapsedTimer m_clock;
    int m_backoffMs = 1000;
    int m_failures = 0;
    bool m_listening = false;
#ifdef Q_OS_WASM
    std::shared_ptr<rtc::WebSocket> m_ws;
#else
    QNetworkReply *m_reply = nullptr;
    QByteArray m_buffer;
#endif
};

Signaling::Signaling(QObject *parent)
    : QObject(parent),
      m_nam(new QNetworkAccessManager(this)),
      m_servers(loadServers())
{
}

Signaling::~Signaling()
{
    qDeleteAll(m_subscriptions);
}

void Signaling::listen(const QString &topic)
{
    qDeleteAll(m_subscriptions);
    m_subscriptions.clear();
    for (int i = 0; i < m_servers.size(); ++i)
        m_subscriptions.push_back(new Subscription(m_nam, i, m_servers.at(i), topic, this));
}

void Signaling::publish(int server, const QString &topic, const QByteArray &body,
                        std::function<void(const QString &error)> done)
{
    QNetworkRequest request(QUrl(QStringLiteral("https://%1/%2").arg(m_servers.at(server), topic)));
    request.setTransferTimeout(kPublishTimeoutMs);
    // text/plain keeps the browser's fetch a "simple" CORS request (no preflight).
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/plain"));
    QNetworkReply *reply = m_nam->post(request, body);
    connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError)
            done(status ? QStringLiteral("HTTP %1: %2").arg(status).arg(reply->errorString()) : reply->errorString());
        else if (status != 200)
            done(QStringLiteral("HTTP %1").arg(status));
        else
            done(QString());
    });
}
