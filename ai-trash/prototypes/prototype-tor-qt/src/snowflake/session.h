#pragma once

// One Snowflake connection for one tor bridge connection, layered exactly like
// the Go client (client/lib/snowflake.go, newSession):
//
//   tor socket  <->  smux v2 stream 3  <->  KCP (stream mode)  <->  encapsulation
//                                                                    <->  WebRTC data channel (a proxy)
//
// The KCP/smux session survives proxy churn: when a proxy goes away, a new
// one is fetched from the broker, the turbotunnel token + client ID are sent
// again, and the server re-attaches the same KCP conversation.
//
// In the browser, after kWebRtcAttemptsBeforeWebSocket proxies in a row fail
// to connect, the next attempt is a WebSocket straight to the bridge
// (BridgeArgs::relayUrl); after that fails too, WebRTC is tried again.

#include "encapsulation.h"
#include "loopback.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

struct IKCPCB;

namespace snowflake {

class Broker;
class Peer;
class WebRtcPeer;

struct BridgeArgs
{
    QUrl brokerUrl;
    QStringList fronts;
    QStringList iceServers;
    QString fingerprint;
    QUrl relayUrl; // the bridge's own WebSocket URL, for the browser fallback
};

class Session : public QObject
{
    Q_OBJECT

public:
    Session(const BridgeArgs &args, LoopbackSocket *torSocket, int id, QObject *parent = nullptr);
    ~Session() override;

    // The app was in the background: make sure the proxy is still there
    // instead of waiting for the silence timeout.
    void checkPeer();

signals:
    void log(const QString &line);

private:
    // proxies
    void requestPeer();
    void watchPeer(Peer *peer);
    void onOffer(WebRtcPeer *peer, const QString &sdp);
    void onPeerOpened(Peer *peer);
    void dropPeer(Peer *peer, const QString &reason);
    void retryPeerLater(int ms);

    // KCP
    static int kcpOutput(const char *buf, int len, IKCPCB *kcp, void *user);
    void sendPacket(const QByteArray &packet);
    void onPeerMessage(const QByteArray &bytes);
    void tick();

    // smux
    void sendFrame(quint8 cmd, const QByteArray &payload = {});
    void pumpFromTor();
    void readFromKcp();
    void handleFrames();
    void onTorBytesWritten(qint64 n);
    void closeFromTor();

    void say(const QString &line);

    int m_id;
    BridgeArgs m_args;
    QPointer<LoopbackSocket> m_tor;
    Broker *m_broker;
    QByteArray m_clientId;

    Peer *m_pending = nullptr; // being negotiated
    Peer *m_current = nullptr; // carrying traffic
    bool m_pendingIsWebSocket = false;
    int m_webRtcFailures = 0;  // in a row
    QTimer m_pendingTimeout;
    QTimer m_retry;
    QTimer m_probe;              // checkPeer(): waiting for any reply
    QElapsedTimer m_probeClock;
    quint32 m_probeSeq = 0;      // KCP snd_nxt after the probe NOP; done when snd_una reaches it
    QElapsedTimer m_lastReceive;
    QString m_natType;
    bool m_gotAnswer = false;
    int m_peerCounter = 0;

    IKCPCB *m_kcp = nullptr;
    QTimer m_tick;
    QElapsedTimer m_clock;
    QElapsedTimer m_lastKeepAlive;
    QList<QByteArray> m_queued; // KCP packets waiting for a proxy (like turbotunnel's queue)
    EncapsulationDecoder m_decoder;

    QByteArray m_smuxIn;
    quint32 m_numWritten = 0;   // bytes sent on stream 3
    quint32 m_peerConsumed = 0; // as reported by the server's UPD
    quint32 m_peerWindow = 262144;
    quint32 m_numRead = 0;      // bytes delivered to tor
    quint32 m_incr = 0;         // since the last UPD we sent
    qint64 m_pendingToTor = 0;  // written to the tor socket, not yet flushed
    bool m_finSent = false;
    bool m_streamClosed = false;
    bool m_trace = false; // SNOWFLAKE_TRACE=1: log smux frames
    quint64 m_bytesUp = 0;
    quint64 m_bytesDown = 0;
};

} // namespace snowflake
