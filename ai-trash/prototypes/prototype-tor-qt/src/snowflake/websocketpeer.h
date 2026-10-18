#pragma once

// A WebSocket straight to the Snowflake bridge (WebAssembly only). It is what a
// volunteer proxy would open on our behalf; the browser can open it itself,
// so it is the fallback when no WebRTC proxy connects.

#include "peer.h"

#include <QUrl>

#include <memory>

namespace rtc {
class WebSocket;
}

namespace snowflake {

class WebSocketPeer : public Peer
{
    Q_OBJECT

public:
    WebSocketPeer(const QUrl &url, const QString &label, QObject *parent = nullptr);
    ~WebSocketPeer() override;

    void open(); // -> opened() or closed()
    bool send(const QByteArray &bytes) override;

private:
    QUrl m_url;
    std::shared_ptr<rtc::WebSocket> m_ws;
};

} // namespace snowflake
