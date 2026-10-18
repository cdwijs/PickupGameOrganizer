#include "broker.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>

namespace snowflake {

namespace {
constexpr qint64 kReadLimit = 100000; // same cap as the Go client
constexpr int kTimeoutMs = 30000;
} // namespace

Broker::Broker(const QUrl &brokerUrl, const QStringList &fronts, QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)), m_brokerUrl(brokerUrl), m_fronts(fronts)
{
}

void Broker::negotiate(const QString &offerSdp, const QString &natType, const QString &fingerprint,
                       Callback callback)
{
    // The offer is a JSON-serialised RTCSessionDescription, itself a string
    // inside the poll request.
    const QByteArray offer = QJsonDocument(QJsonObject{{QStringLiteral("type"), QStringLiteral("offer")},
                                                       {QStringLiteral("sdp"), offerSdp}})
                                 .toJson(QJsonDocument::Compact);
    QJsonObject request{{QStringLiteral("offer"), QString::fromUtf8(offer)},
                        {QStringLiteral("nat"), natType}};
    if (!fingerprint.isEmpty())
        request.insert(QStringLiteral("fingerprint"), fingerprint);
    const QByteArray body = "1.0\n" + QJsonDocument(request).toJson(QJsonDocument::Compact);

    QUrl url = m_brokerUrl.resolved(QUrl(QStringLiteral("client")));
    const QString realHost = url.host();
    QNetworkRequest req;
    if (!m_fronts.isEmpty()) {
        const QString front = m_fronts.at(QRandomGenerator::global()->bounded(m_fronts.size()));
        url.setHost(front);
        req.setRawHeader("Host", realHost.toLatin1());
        emit log(QStringLiteral("Broker: POST via front %1 (Host: %2)").arg(front, realHost));
    } else {
        emit log(QStringLiteral("Broker: POST %1").arg(url.toString()));
    }
    req.setUrl(url);
    // The Host override only behaves on HTTP/1.1; HTTP/2 uses :authority.
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/x-www-form-urlencoded"));
    req.setTransferTimeout(kTimeoutMs);

    QNetworkReply *reply = m_network->post(req, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
#ifdef Q_OS_WASM
        // CDN77's own error pages (e.g. 504 when the broker found no proxy in
        // time) carry no CORS headers, so the page only sees status 0.
        if (status == 0) {
            callback({}, QStringLiteral("broker: no readable response (an error without CORS headers, "
                                        "usually a CDN timeout because no proxy was available)"));
            return;
        }
#endif
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            callback({}, QStringLiteral("broker HTTP %1: %2").arg(status).arg(reply->errorString()));
            return;
        }
        const QByteArray data = reply->read(kReadLimit);
        const QJsonObject response = QJsonDocument::fromJson(data).object();
        const QString error = response.value(QLatin1String("error")).toString();
        if (!error.isEmpty()) {
            callback({}, QStringLiteral("broker: ") + error);
            return;
        }
        const QJsonObject answer =
            QJsonDocument::fromJson(response.value(QLatin1String("answer")).toString().toUtf8()).object();
        const QString sdp = answer.value(QLatin1String("sdp")).toString();
        if (answer.value(QLatin1String("type")).toString() != QLatin1String("answer") || sdp.isEmpty()) {
            callback({}, QStringLiteral("broker: malformed answer"));
            return;
        }
        emit log(QStringLiteral("Broker: got an answer from a proxy"));
        callback(sdp, {});
    });
}

} // namespace snowflake
