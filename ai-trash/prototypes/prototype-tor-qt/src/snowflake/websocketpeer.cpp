#include "websocketpeer.h"

#include <rtc/websocket.hpp>

namespace snowflake {

WebSocketPeer::WebSocketPeer(const QUrl &url, const QString &label, QObject *parent)
    : Peer(label, parent), m_url(url), m_ws(std::make_shared<rtc::WebSocket>())
{
    // datachannel-wasm calls back on the browser's main thread (this thread);
    // queueing keeps the Session from being re-entered mid-call.
    m_ws->onOpen([this] { QMetaObject::invokeMethod(this, [this] { markOpen(); }, Qt::QueuedConnection); });
    m_ws->onClosed([this] {
        QMetaObject::invokeMethod(this, [this] { fail(QStringLiteral("WebSocket closed")); }, Qt::QueuedConnection);
    });
    m_ws->onError([this](std::string error) {
        QMetaObject::invokeMethod(this, [this, e = QString::fromStdString(error)] {
            fail(QStringLiteral("WebSocket error: ") + e);
        }, Qt::QueuedConnection);
    });
    m_ws->onMessage([this](rtc::message_variant data) {
        QByteArray bytes;
        if (const auto *bin = std::get_if<rtc::binary>(&data))
            bytes = QByteArray(reinterpret_cast<const char *>(bin->data()), qsizetype(bin->size()));
        else
            bytes = QByteArray::fromStdString(std::get<std::string>(data));
        QMetaObject::invokeMethod(this, [this, bytes] {
            if (!isClosed())
                emit message(bytes);
        }, Qt::QueuedConnection);
    });
}

WebSocketPeer::~WebSocketPeer()
{
    m_ws->close();
}

void WebSocketPeer::open()
{
    emit log(QStringLiteral("opening WebSocket to %1").arg(m_url.toString()));
    m_ws->open(m_url.toString().toStdString());
}

bool WebSocketPeer::send(const QByteArray &bytes)
{
    if (!isOpen())
        return false;
    return m_ws->send(reinterpret_cast<const std::byte *>(bytes.constData()), size_t(bytes.size()));
}

} // namespace snowflake
