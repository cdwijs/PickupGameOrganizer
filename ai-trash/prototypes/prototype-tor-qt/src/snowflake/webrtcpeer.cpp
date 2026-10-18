#include "webrtcpeer.h"

#include <QHostAddress>
#include <QMap>
#include <QRandomGenerator>

#include <rtc/rtc.hpp>

namespace snowflake {

WebRtcPeer::WebRtcPeer(const QStringList &iceServers, const QString &label, QObject *parent)
    : Peer(label, parent)
{
    rtc::Configuration config;
#ifdef Q_OS_WASM
    // The browser's ICE agent uses every STUN server it is given.
    for (const QString &server : iceServers)
        config.iceServers.emplace_back(server.toStdString());
#else
    // libjuice handles a single STUN server; pick one at random per peer.
    if (!iceServers.isEmpty()) {
        const QString server = iceServers.at(QRandomGenerator::global()->bounded(iceServers.size()));
        config.iceServers.emplace_back(server.toStdString());
    }
#endif
    m_pc = std::make_shared<rtc::PeerConnection>(config);

    m_pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
        if (state != rtc::PeerConnection::GatheringState::Complete)
            return;
        QMetaObject::invokeMethod(this, [this] {
            if (m_offerSent || isClosed())
                return;
            const auto description = m_pc->localDescription();
            if (!description) {
                fail(QStringLiteral("no local description"));
                return;
            }
            m_offerSent = true;
            const QString sdp = stripLocalCandidates(QString::fromStdString(std::string(*description)));
            emit log(QStringLiteral("offer candidates: ") + candidateSummary(sdp));
            emit offerReady(sdp);
        }, Qt::QueuedConnection);
    });
    m_pc->onStateChange([this](rtc::PeerConnection::State state) {
        if (state != rtc::PeerConnection::State::Failed && state != rtc::PeerConnection::State::Closed)
            return;
        const QString reason = state == rtc::PeerConnection::State::Failed ? QStringLiteral("ICE/DTLS failed")
                                                                           : QStringLiteral("peer closed");
        QMetaObject::invokeMethod(this, [this, reason] { fail(reason); }, Qt::QueuedConnection);
    });
}

WebRtcPeer::~WebRtcPeer()
{
#ifdef Q_OS_WASM
    // datachannel-wasm calls back on this thread only, and destroying the
    // objects detaches their JS handlers; anything already queued to `this`
    // is dropped together with it.
    if (m_dc)
        m_dc->close();
    m_pc->close();
#else
    // Resetting waits for any callback in flight, so none can touch `this` afterwards.
    if (m_dc) {
        m_dc->resetCallbacks();
        m_dc->close();
    }
    m_pc->resetCallbacks();
    m_pc->close();
#endif
}

void WebRtcPeer::createOffer()
{
    rtc::DataChannelInit init; // ordered + reliable, as in the Go client
    m_dc = m_pc->createDataChannel(label().toStdString(), init); // starts negotiation and ICE gathering

    m_dc->onOpen([this] {
        QMetaObject::invokeMethod(this, [this] { markOpen(); }, Qt::QueuedConnection);
    });
    m_dc->onClosed([this] {
        QMetaObject::invokeMethod(this, [this] { fail(QStringLiteral("data channel closed")); },
                                  Qt::QueuedConnection);
    });
    m_dc->onError([this](std::string error) {
        QMetaObject::invokeMethod(this, [this, e = QString::fromStdString(error)] {
            fail(QStringLiteral("data channel error: ") + e);
        }, Qt::QueuedConnection);
    });
    m_dc->onMessage([this](rtc::message_variant data) {
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

void WebRtcPeer::setAnswer(const QString &answerSdp)
{
    emit log(QStringLiteral("answer candidates: ") + candidateSummary(answerSdp));
    try {
        m_pc->setRemoteDescription(rtc::Description(answerSdp.toStdString(), rtc::Description::Type::Answer));
    } catch (const std::exception &e) {
        fail(QStringLiteral("bad answer: ") + QString::fromUtf8(e.what()));
    }
}

bool WebRtcPeer::send(const QByteArray &bytes)
{
    if (!isOpen() || !m_dc)
        return false;
    try {
        const auto *begin = reinterpret_cast<const std::byte *>(bytes.constData());
        return m_dc->send(begin, size_t(bytes.size()));
    } catch (const std::exception &e) {
        fail(QStringLiteral("send failed: ") + QString::fromUtf8(e.what()));
        return false;
    }
}

QString candidateSummary(const QString &sdp)
{
    QMap<QString, int> types;
    for (const QString &line : sdp.split(QStringLiteral("\r\n"))) {
        if (!line.startsWith(QLatin1String("a=candidate:")))
            continue;
        const QStringList f = line.split(QLatin1Char(' '));
        types[f.value(7, QStringLiteral("?"))]++;
    }
    if (types.isEmpty())
        return QStringLiteral("none");
    QStringList parts;
    for (auto it = types.cbegin(); it != types.cend(); ++it)
        parts << QStringLiteral("%1 %2").arg(it.value()).arg(it.key());
    return parts.join(QStringLiteral(", "));
}

QString stripLocalCandidates(const QString &sdp)
{
    const QStringList lines = sdp.split(QStringLiteral("\r\n"));
    QStringList kept;
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("a=candidate:"))) {
            // a=candidate:<foundation> <component> <transport> <priority> <address> <port> typ <type>
            const QStringList f = line.split(QLatin1Char(' '));
            if (f.size() >= 8 && f.at(7) == QLatin1String("host")) {
                const QHostAddress addr(f.at(4));
                if (addr.isNull() || addr.isLoopback() || addr.isPrivateUse() || addr.isLinkLocal()
                    || addr.isUniqueLocalUnicast() || addr == QHostAddress::AnyIPv4 || addr == QHostAddress::AnyIPv6)
                    continue;
            }
        }
        kept << line;
    }
    return kept.join(QStringLiteral("\r\n"));
}

} // namespace snowflake
