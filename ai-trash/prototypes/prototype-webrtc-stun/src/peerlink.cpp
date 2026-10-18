#include "peerlink.h"

#include <QMap>

#include <rtc/rtc.hpp>

namespace {

// After the first public candidate, wait this long for the others (more
// srflx from other interfaces, IPv6) before sending the description.
constexpr int kGraceMs = 300;

// "candidate:<foundation> <component> <transport> <priority> <address> <port> typ <type> ..."
QStringList candidateFields(QString candidate)
{
    if (candidate.startsWith(QLatin1String("a=")))
        candidate.remove(0, 2);
    return candidate.split(QLatin1Char(' '));
}

QString candidateType(const QString &candidate)
{
    return candidateFields(candidate).value(7, QStringLiteral("?"));
}

#ifndef Q_OS_WASM
QString describeCandidate(const rtc::Candidate &candidate)
{
    const QStringList f = candidateFields(QString::fromStdString(candidate.candidate()));
    return QStringLiteral("%1 %2:%3").arg(f.value(7), f.value(4), f.value(5));
}
#endif

} // namespace

PeerLink::PeerLink(const QString &stunServer, int gatherTimeoutMs, QObject *parent)
    : QObject(parent),
      m_stunServer(stunServer)
{
    m_clock.start();
    rtc::Configuration config;
    if (!stunServer.isEmpty())
        config.iceServers.emplace_back(QStringLiteral("stun:%1").arg(stunServer).toStdString());
    m_pc = std::make_shared<rtc::PeerConnection>(config);

    m_graceTimer.setSingleShot(true);
    m_graceTimer.setInterval(kGraceMs);
    connect(&m_graceTimer, &QTimer::timeout, this, &PeerLink::finishGathering);
    m_gatherTimer.setSingleShot(true);
    m_gatherTimer.setInterval(gatherTimeoutMs);
    connect(&m_gatherTimer, &QTimer::timeout, this, &PeerLink::finishGathering);

    m_pc->onLocalDescription([this](rtc::Description description) {
        QMetaObject::invokeMethod(this, [this, sdp = QString::fromStdString(std::string(description))] {
            m_localSdp = sdp;
            if (m_wantFinish)
                finishGathering();
        }, Qt::QueuedConnection);
    });
    m_pc->onLocalCandidate([this](rtc::Candidate candidate) {
        QMetaObject::invokeMethod(this, [this, c = QString::fromStdString(candidate.candidate())] {
            addLocalCandidate(c);
        }, Qt::QueuedConnection);
    });
    m_pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
        if (state != rtc::PeerConnection::GatheringState::Complete)
            return;
        QMetaObject::invokeMethod(this, &PeerLink::finishGathering, Qt::QueuedConnection);
    });
    m_pc->onIceStateChange([this](rtc::PeerConnection::IceState state) {
        static const char *const names[] = {"new", "checking", "connected", "completed",
                                            "failed", "disconnected", "closed"};
        const int i = int(state);
        if (i < 0 || i > 6)
            return;
        QMetaObject::invokeMethod(this, [this, name = QString::fromLatin1(names[i])] {
            emit log(QStringLiteral("ICE ") + name);
        }, Qt::QueuedConnection);
    });
    m_pc->onStateChange([this](rtc::PeerConnection::State state) {
        if (state == rtc::PeerConnection::State::Connected) {
            QString pair;
#ifndef Q_OS_WASM
            // The browser does not expose the selected pair through datachannel-wasm.
            rtc::Candidate local, remote;
            if (m_pc->getSelectedCandidatePair(&local, &remote))
                pair = QStringLiteral(": local %1 <-> remote %2").arg(describeCandidate(local), describeCandidate(remote));
#endif
            QMetaObject::invokeMethod(this, [this, pair] {
                emit log(QStringLiteral("DTLS connected") + pair);
            }, Qt::QueuedConnection);
            return;
        }
        if (state != rtc::PeerConnection::State::Failed && state != rtc::PeerConnection::State::Closed)
            return;
        const QString reason = state == rtc::PeerConnection::State::Failed ? QStringLiteral("ICE/DTLS failed")
                                                                           : QStringLiteral("peer connection closed");
        QMetaObject::invokeMethod(this, [this, reason] { fail(reason); }, Qt::QueuedConnection);
    });
    // The answering side gets the channel the offerer created.
    m_pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel) {
        QMetaObject::invokeMethod(this, [this, channel] { attachChannel(channel); }, Qt::QueuedConnection);
    });
}

PeerLink::~PeerLink()
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

void PeerLink::createOffer()
{
    m_gatherTimer.start();
    rtc::DataChannelInit init; // ordered + reliable
    auto channel = m_pc->createDataChannel("chat", init); // starts negotiation and ICE gathering
    attachChannel(channel);
}

void PeerLink::acceptOffer(const QString &offer, bool holdCandidates)
{
    m_gatherTimer.start();
    QString sdp = offer;
    if (holdCandidates) {
        QStringList kept;
        for (const QString &line : offer.split(QStringLiteral("\r\n"))) {
            if (line.startsWith(QLatin1String("a=candidate:")))
                m_heldCandidates << line.mid(2);
            else if (line == QLatin1String("a=end-of-candidates"))
                continue;
            else
                kept << line;
            if (line.startsWith(QLatin1String("a=mid:")))
                m_remoteMid = line.mid(6);
        }
        sdp = kept.join(QStringLiteral("\r\n"));
    }
    try {
        // The answer is created automatically and arrives via onLocalDescription.
        m_pc->setRemoteDescription(rtc::Description(sdp.toStdString(), "offer"));
    } catch (const std::exception &e) {
        fail(QStringLiteral("bad offer: ") + QString::fromUtf8(e.what()));
    }
}

void PeerLink::releaseRemoteCandidates()
{
    if (m_closed || m_heldCandidates.isEmpty())
        return;
    const std::string mid = (m_remoteMid.isEmpty() ? QStringLiteral("0") : m_remoteMid).toStdString();
    try {
        for (const QString &candidate : std::as_const(m_heldCandidates))
            m_pc->addRemoteCandidate(rtc::Candidate(candidate.toStdString(), mid));
    } catch (const std::exception &e) {
        fail(QStringLiteral("bad remote candidate: ") + QString::fromUtf8(e.what()));
    }
    m_heldCandidates.clear();
}

void PeerLink::setAnswer(const QString &answer)
{
    try {
        m_pc->setRemoteDescription(rtc::Description(answer.toStdString(), "answer"));
    } catch (const std::exception &e) {
        fail(QStringLiteral("bad answer: ") + QString::fromUtf8(e.what()));
    }
}

bool PeerLink::send(const QString &text)
{
    if (!m_open || !m_dc)
        return false;
    try {
        return m_dc->send(text.toStdString());
    } catch (const std::exception &e) {
        fail(QStringLiteral("send failed: ") + QString::fromUtf8(e.what()));
        return false;
    }
}

void PeerLink::attachChannel(std::shared_ptr<rtc::DataChannel> channel)
{
    if (m_dc && m_dc != channel)
        return; // only the one "chat" channel
    m_dc = channel;
    m_dc->onOpen([this] {
        QMetaObject::invokeMethod(this, [this] {
            if (m_open || m_closed)
                return;
            m_open = true;
            emit opened();
        }, Qt::QueuedConnection);
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
        QString text;
        if (const auto *bin = std::get_if<rtc::binary>(&data))
            text = QString::fromUtf8(reinterpret_cast<const char *>(bin->data()), qsizetype(bin->size()));
        else
            text = QString::fromStdString(std::get<std::string>(data));
        QMetaObject::invokeMethod(this, [this, text] {
            if (!m_closed)
                emit message(text);
        }, Qt::QueuedConnection);
    });
    // The answering side may get the channel when it is already open.
    if (m_dc->isOpen() && !m_open) {
        m_open = true;
        emit opened();
    }
}

void PeerLink::addLocalCandidate(const QString &candidate)
{
    if (m_ready)
        return;
    m_candidates << candidate;
    if (candidateType(candidate) == QLatin1String("srflx") && !m_hasSrflx) {
        m_hasSrflx = true;
        m_graceTimer.start();
    }
}

void PeerLink::finishGathering()
{
    if (m_ready || m_closed)
        return;
    if (m_localSdp.isEmpty()) {
        m_wantFinish = true; // the description itself has not arrived yet
        return;
    }
    m_ready = true;
    m_graceTimer.stop();
    m_gatherTimer.stop();

    // Inline the candidates gathered so far (the description may or may not
    // carry some already). With a single m= section, the end of the SDP is
    // inside it.
    QString sdp = m_localSdp;
    if (!sdp.endsWith(QLatin1String("\r\n")))
        sdp += QLatin1String("\r\n");
    for (const QString &candidate : std::as_const(m_candidates)) {
        const QString line = candidate.startsWith(QLatin1String("a=")) ? candidate : QStringLiteral("a=") + candidate;
        if (!sdp.contains(line))
            sdp += line + QLatin1String("\r\n");
    }
    emit localReady(sdp, m_hasSrflx);
}

void PeerLink::fail(const QString &reason)
{
    if (m_closed)
        return;
    m_closed = true;
    m_open = false;
    m_graceTimer.stop();
    m_gatherTimer.stop();
    emit closed(reason);
}

QString candidateSummary(const QString &sdp)
{
    QMap<QString, int> types;
    for (const QString &line : sdp.split(QStringLiteral("\r\n"))) {
        if (line.startsWith(QLatin1String("a=candidate:")))
            types[candidateType(line)]++;
    }
    if (types.isEmpty())
        return QStringLiteral("none");
    QStringList parts;
    for (auto it = types.cbegin(); it != types.cend(); ++it)
        parts << QStringLiteral("%1 %2").arg(it.value()).arg(it.key());
    return parts.join(QStringLiteral(", "));
}
