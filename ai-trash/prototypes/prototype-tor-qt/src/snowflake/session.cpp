#include "session.h"

#include "broker.h"
#include "webrtcpeer.h"
#ifdef Q_OS_WASM
#include "websocketpeer.h"
#endif

#include "ikcp.h"

#include <QRandomGenerator>
#include <QtEndian>

#include <algorithm>

namespace snowflake {

namespace {

// Constants mirrored from the Go client (client/lib/snowflake.go) and the
// libraries it uses (smux, kcp-go, common/turbotunnel).
constexpr char kToken[8] = {'\x12', '\x93', '\x60', '\x5d', '\x27', '\x81', '\x75', '\xf5'};
constexpr int kKcpWindow = 65535;
constexpr int kTickMs = 10;
constexpr int kQueueLimit = 512;             // turbotunnel queueSize
constexpr int kSnowflakeTimeoutMs = 20000;   // drop a proxy that has been silent this long
constexpr int kDataChannelTimeoutMs = 10000; // answer received -> channel must open
constexpr int kNegotiateTimeoutMs = 45000;   // gathering + broker + open
constexpr int kReconnectMs = 10000;          // ReconnectTimeout
constexpr int kProbeMs = 5000;               // checkPeer(): a live proxy answers well within this
#ifdef Q_OS_WASM
constexpr int kWebRtcAttemptsBeforeWebSocket = 2;
constexpr int kWebSocketTimeoutMs = 15000;   // open the WebSocket to the bridge
#endif

constexpr quint8 kSmuxVersion = 2;
constexpr quint8 cmdSYN = 0, cmdFIN = 1, cmdPSH = 2, cmdNOP = 3, cmdUPD = 4;
constexpr quint32 kStreamId = 3;             // first stream a smux client opens
constexpr int kMaxFrame = 32768;             // smux MaxFrameSize
constexpr quint32 kStreamBuffer = 1048576;   // StreamSize, our advertised window
constexpr int kKeepAliveMs = 10000;          // smux KeepAliveInterval
constexpr int kMaxKcpBacklog = 1024;         // packets; pause reading tor beyond this

QString natAfterFailure() { return QStringLiteral("unknown"); }

} // namespace

Session::Session(const BridgeArgs &args, LoopbackSocket *torSocket, int id, QObject *parent)
    : QObject(parent), m_id(id), m_args(args), m_tor(torSocket),
      m_broker(new Broker(args.brokerUrl, args.fronts, this))
{
    connect(m_broker, &Broker::log, this, &Session::say);

    // Unknown NAT is reported as "unrestricted" until a proxy fails to
    // connect, the same optimistic policy as the Go client's NATPolicy.
    m_natType = QStringLiteral("unrestricted");

    m_trace = qEnvironmentVariableIsSet("SNOWFLAKE_TRACE");
    m_clientId.resize(8);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(m_clientId.data()), 2);

    m_kcp = ikcp_create(QRandomGenerator::system()->generate(), this);
    m_kcp->output = &Session::kcpOutput;
    ikcp_wndsize(m_kcp, kKcpWindow, kKcpWindow);
    ikcp_nodelay(m_kcp, 0, 0, 0, 1); // kcp-go SetNoDelay(0, 0, 0, 1): congestion window off
    m_kcp->stream = 1;               // SetStreamMode(true)
    m_clock.start();
    m_lastKeepAlive.start();

    torSocket->setParent(this);
    connect(torSocket, &LoopbackSocket::readyRead, this, &Session::pumpFromTor);
    connect(torSocket, &LoopbackSocket::bytesWritten, this, &Session::onTorBytesWritten);
    connect(torSocket, &LoopbackSocket::disconnected, this, &Session::closeFromTor);

    m_pendingTimeout.setSingleShot(true);
    connect(&m_pendingTimeout, &QTimer::timeout, this, [this] {
        if (!m_pending)
            return;
        if (m_pendingIsWebSocket)
            dropPeer(m_pending, QStringLiteral("timeout opening the WebSocket"));
        else
            dropPeer(m_pending, m_gotAnswer ? QStringLiteral("timeout waiting for DataChannel.OnOpen")
                                            : QStringLiteral("timeout negotiating with the broker"));
    });
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &Session::requestPeer);
    connect(&m_tick, &QTimer::timeout, this, &Session::tick);
    m_tick.start(kTickMs);
    m_probe.setSingleShot(true);
    connect(&m_probe, &QTimer::timeout, this, [this] {
        dropPeer(m_current, QStringLiteral("no reply within %1 s after returning to the foreground").arg(kProbeMs / 1000));
    });

    // Start on the next event-loop turn so the creator can connect log() first.
    QTimer::singleShot(0, this, [this] {
        say(QStringLiteral("new session, client ID %1").arg(QString::fromLatin1(m_clientId.toHex())));
        sendFrame(cmdSYN); // open stream 3; queued until a proxy is connected
        requestPeer();
    });
}

Session::~Session()
{
    delete m_pending;
    delete m_current;
    ikcp_release(m_kcp);
}

void Session::say(const QString &line)
{
    emit log(QStringLiteral("[snowflake %1] ").arg(m_id) + line);
}

// --- proxies -----------------------------------------------------------------

void Session::requestPeer()
{
    if (m_pending || m_current || m_streamClosed)
        return;
    m_gotAnswer = false;
    const QString label = QStringLiteral("snowflake-%1").arg(++m_peerCounter);

#ifdef Q_OS_WASM
    if (m_webRtcFailures >= kWebRtcAttemptsBeforeWebSocket && m_args.relayUrl.isValid()) {
        say(QStringLiteral("%1 proxies in a row failed; connecting straight to the bridge").arg(m_webRtcFailures));
        auto *ws = new WebSocketPeer(m_args.relayUrl, label, this);
        m_pendingIsWebSocket = true;
        watchPeer(ws);
        m_pendingTimeout.start(kWebSocketTimeoutMs);
        ws->open();
        return;
    }
#endif

    auto *peer = new WebRtcPeer(m_args.iceServers, label, this);
    m_pendingIsWebSocket = false;
    watchPeer(peer);
    connect(peer, &WebRtcPeer::offerReady, this, [this, peer](const QString &sdp) { onOffer(peer, sdp); });
    say(QStringLiteral("gathering ICE candidates for a new proxy connection"));
    m_pendingTimeout.start(kNegotiateTimeoutMs);
    peer->createOffer();
}

void Session::checkPeer()
{
    if (m_streamClosed)
        return;
    if (m_current) {
        // Only the bridge's ACK for this NOP counts: bytes that were already
        // buffered while we were suspended arrive at once and prove nothing.
        say(QStringLiteral("back in the foreground: checking %1 (silent for %2 s)")
                .arg(m_current->label())
                .arg(m_lastReceive.elapsed() / 1000));
        sendFrame(cmdNOP);
        ikcp_flush(m_kcp);
        m_probeSeq = m_kcp->snd_nxt;
        m_lastKeepAlive.start();
        m_probeClock.start();
        m_probe.start(kProbeMs);
    } else if (!m_pending && m_retry.isActive()) {
        say(QStringLiteral("back in the foreground: fetching a proxy now instead of in %1 s")
                .arg(m_retry.remainingTime() / 1000));
        m_retry.start(0);
    }
}

void Session::watchPeer(Peer *peer)
{
    m_pending = peer;
    connect(peer, &Peer::log, this, &Session::say);
    connect(peer, &Peer::opened, this, [this, peer] { onPeerOpened(peer); });
    connect(peer, &Peer::message, this, &Session::onPeerMessage);
    connect(peer, &Peer::closed, this, [this, peer](const QString &reason) { dropPeer(peer, reason); });
}

void Session::onOffer(WebRtcPeer *peer, const QString &sdp)
{
    say(QStringLiteral("offer ready, asking the broker for a proxy (nat=%1)").arg(m_natType));
    QPointer<WebRtcPeer> guard(peer);
    m_broker->negotiate(sdp, m_natType, m_args.fingerprint, [this, guard](const QString &answer, const QString &error) {
        if (!guard || guard != m_pending)
            return;
        if (!error.isEmpty()) {
            dropPeer(guard, error);
            return;
        }
        m_gotAnswer = true;
        m_pendingTimeout.start(kDataChannelTimeoutMs);
        guard->setAnswer(answer);
    });
}

void Session::onPeerOpened(Peer *peer)
{
    if (peer != m_pending)
        return;
    m_pendingTimeout.stop();
    m_pending = nullptr;
    m_current = peer;
    m_lastReceive.start();
    m_decoder.clear();
    if (m_pendingIsWebSocket) {
        say(QStringLiteral("connected straight to the bridge over WebSocket (%1)").arg(peer->label()));
    } else {
        m_webRtcFailures = 0;
        say(QStringLiteral("connected to a Snowflake proxy (%1)").arg(peer->label()));
    }

    // turbotunnel: token + client ID let the server attach this proxy to our KCP session.
    peer->send(QByteArray(kToken, sizeof kToken));
    peer->send(m_clientId);
    for (const QByteArray &packet : std::exchange(m_queued, {}))
        peer->send(encapsulate(packet));
}

void Session::dropPeer(Peer *peer, const QString &reason)
{
    if (!peer)
        return;
    if (peer == m_pending) {
        m_pendingTimeout.stop();
        m_pending = nullptr;
        // Alternate: a failed WebSocket gives WebRTC another round.
        m_webRtcFailures = m_pendingIsWebSocket ? 0 : m_webRtcFailures + 1;
        if (m_gotAnswer && m_natType != natAfterFailure()) {
            say(QStringLiteral("proxy did not connect; reporting NAT as unknown from now on"));
            m_natType = natAfterFailure();
        }
        say(QStringLiteral("proxy attempt failed: %1, retrying in %2 s").arg(reason).arg(kReconnectMs / 1000));
        retryPeerLater(kReconnectMs);
    } else if (peer == m_current) {
        m_probe.stop();
        m_current = nullptr;
        say(QStringLiteral("proxy connection lost: %1, fetching a new proxy").arg(reason));
        retryPeerLater(0);
    }
    peer->disconnect(this);
    peer->deleteLater();
}

void Session::retryPeerLater(int ms)
{
    if (!m_streamClosed)
        m_retry.start(ms);
}

// --- KCP ---------------------------------------------------------------------

int Session::kcpOutput(const char *buf, int len, IKCPCB *, void *user)
{
    static_cast<Session *>(user)->sendPacket(QByteArray(buf, len));
    return 0;
}

void Session::sendPacket(const QByteArray &packet)
{
    if (m_current && m_current->isOpen()) {
        m_current->send(encapsulate(packet));
        return;
    }
    if (m_queued.size() < kQueueLimit)
        m_queued << packet; // KCP will retransmit anything dropped beyond this
}

void Session::onPeerMessage(const QByteArray &bytes)
{
    if (sender() != m_current)
        return;
    m_lastReceive.start();
    m_decoder.feed(bytes);
    QByteArray packet;
    bool error = false;
    while (m_decoder.next(&packet, &error))
        ikcp_input(m_kcp, packet.constData(), long(packet.size()));
    if (m_probe.isActive() && qint32(m_kcp->snd_una - m_probeSeq) >= 0) {
        m_probe.stop();
        say(QStringLiteral("%1 is still connected (bridge acknowledged after %2 ms)")
                .arg(m_current->label())
                .arg(m_probeClock.elapsed()));
    }
    if (error)
        dropPeer(m_current, QStringLiteral("malformed encapsulation"));
    readFromKcp();
}

void Session::tick()
{
    ikcp_update(m_kcp, quint32(m_clock.elapsed()));
    readFromKcp();
    pumpFromTor();

    if (m_lastKeepAlive.elapsed() >= kKeepAliveMs) {
        sendFrame(cmdNOP);
        m_lastKeepAlive.start();
    }
    if (m_current && m_lastReceive.elapsed() > kSnowflakeTimeoutMs)
        dropPeer(m_current, QStringLiteral("no data for %1 s").arg(kSnowflakeTimeoutMs / 1000));
}

// --- smux --------------------------------------------------------------------

void Session::sendFrame(quint8 cmd, const QByteArray &payload)
{
    QByteArray frame(8, '\0');
    frame[0] = char(kSmuxVersion);
    frame[1] = char(cmd);
    qToLittleEndian<quint16>(quint16(payload.size()), frame.data() + 2);
    qToLittleEndian<quint32>(cmd == cmdNOP ? 0 : kStreamId, frame.data() + 4);
    frame += payload;
    if (m_trace && cmd != cmdNOP)
        say(QStringLiteral("smux -> cmd=%1 len=%2").arg(cmd).arg(payload.size()));
    ikcp_send(m_kcp, frame.constData(), int(frame.size()));
}

void Session::pumpFromTor()
{
    if (!m_tor || m_finSent)
        return;
    while (m_tor->bytesAvailable() > 0) {
        const qint32 inflight = qint32(m_numWritten - m_peerConsumed);
        const qint64 window = qint64(m_peerWindow) - inflight;
        if (window <= 0 || ikcp_waitsnd(m_kcp) > kMaxKcpBacklog)
            return; // resumes from tick() once the server catches up
        const QByteArray chunk = m_tor->read(std::min({window, qint64(kMaxFrame), m_tor->bytesAvailable()}));
        if (chunk.isEmpty())
            return;
        sendFrame(cmdPSH, chunk);
        m_numWritten += quint32(chunk.size());
        m_bytesUp += quint64(chunk.size());
    }
}

void Session::readFromKcp()
{
    for (;;) {
        const int size = ikcp_peeksize(m_kcp);
        if (size <= 0)
            break;
        QByteArray buf(size, Qt::Uninitialized);
        const int n = ikcp_recv(m_kcp, buf.data(), size);
        if (n <= 0)
            break;
        buf.truncate(n);
        m_smuxIn += buf;
    }
    handleFrames();
}

void Session::handleFrames()
{
    while (m_smuxIn.size() >= 8) {
        const auto *h = reinterpret_cast<const uchar *>(m_smuxIn.constData());
        const quint8 version = h[0];
        const quint8 cmd = h[1];
        const quint16 length = qFromLittleEndian<quint16>(h + 2);
        const quint32 sid = qFromLittleEndian<quint32>(h + 4);
        if (version != kSmuxVersion) {
            say(QStringLiteral("smux protocol error (version %1)").arg(version));
            if (m_tor)
                m_tor->abort();
            return;
        }
        if (m_smuxIn.size() < 8 + length)
            return;
        const QByteArray payload = m_smuxIn.mid(8, length);
        m_smuxIn.remove(0, 8 + length);
        if (m_trace && cmd != cmdNOP)
            say(QStringLiteral("smux <- cmd=%1 sid=%2 len=%3").arg(cmd).arg(sid).arg(length));

        switch (cmd) {
        case cmdPSH:
            if (sid == kStreamId && m_tor && !payload.isEmpty()) {
                m_pendingToTor += payload.size();
                m_bytesDown += quint64(payload.size());
                m_tor->write(payload);
            }
            break;
        case cmdUPD:
            if (sid == kStreamId && payload.size() == 8) {
                m_peerConsumed = qFromLittleEndian<quint32>(payload.constData());
                m_peerWindow = qFromLittleEndian<quint32>(payload.constData() + 4);
            }
            break;
        case cmdFIN:
            if (sid == kStreamId) {
                say(QStringLiteral("server closed the stream (sent %1 B, server consumed %2 B)")
                        .arg(m_numWritten)
                        .arg(m_peerConsumed));
                m_streamClosed = true;
                if (m_tor)
                    m_tor->disconnectFromHost();
            }
            break;
        case cmdNOP:
        case cmdSYN:
        default:
            break;
        }
    }
}

void Session::onTorBytesWritten(qint64 n)
{
    // Only count data as consumed once tor actually took it, so the server's
    // view of our window reflects real backpressure.
    // Only bytes that came from smux count: the SOCKS reply Client wrote on
    // this socket also reports bytesWritten, and over-reporting "consumed"
    // makes the server's smux fail every write with ErrConsumed.
    n = qMin(n, m_pendingToTor);
    if (n <= 0)
        return;
    m_pendingToTor -= n;
    const bool first = m_numRead == 0;
    m_numRead += quint32(n);
    m_incr += quint32(n);
    if (first || m_incr >= kStreamBuffer / 2) {
        QByteArray upd(8, '\0');
        qToLittleEndian<quint32>(m_numRead, upd.data());
        qToLittleEndian<quint32>(kStreamBuffer, upd.data() + 4);
        sendFrame(cmdUPD, upd);
        m_incr = 0;
    }
}

void Session::closeFromTor()
{
    if (!m_finSent) {
        m_finSent = true;
        sendFrame(cmdFIN);
    }
    m_streamClosed = true;
    say(QStringLiteral("tor closed the connection (%1 kB up, %2 kB down)")
            .arg(m_bytesUp / 1024)
            .arg(m_bytesDown / 1024));
    m_retry.stop();
    // Give KCP a moment to deliver the FIN, then tear everything down.
    QTimer::singleShot(3000, this, &QObject::deleteLater);
}

} // namespace snowflake
