#include "connector.h"

#include "nodename.h"
#include "peerlink.h"
#include "signaling.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRandomGenerator>

namespace {

constexpr int kGatherTimeoutMs = 2000;
// Dead STUN servers whose name no longer resolves fail within milliseconds,
// unreachable ones take the full 2 s: keep picking new ones for up to 8 s.
constexpr int kStunBudgetMs = 8000;
constexpr int kMaxStunTries = 10;
constexpr int kAnswerTimeoutMs = 15000;
constexpr int kOpenTimeoutMs = 15000;      // answerer
constexpr int kCallerOpenTimeoutMs = 8000; // caller: fail fast, the next attempt flips the order
// ntfy.sh turns a larger body into an attachment.
constexpr int kMaxBodyBytes = 4000;

// On attempts where the caller sends first, the answerer holds the caller's
// candidates until this long after its answer is posted.
// WEBRTC_STUN_ANSWER_DELAY_MS overrides it.
int holdMs()
{
    bool ok = false;
    const int ms = qEnvironmentVariableIntValue("WEBRTC_STUN_ANSWER_DELAY_MS", &ok);
    return ok ? ms : 2000;
}

QString randomSid()
{
    return QString::number(QRandomGenerator::global()->generate(), 16).rightJustified(8, QLatin1Char('0'));
}

} // namespace

Connector::Connector(QObject *parent)
    : QObject(parent),
      m_signaling(new Signaling(this))
{
    m_answerTimer.setSingleShot(true);
    connect(&m_answerTimer, &QTimer::timeout, this, [this] {
        m_waitingForAnswer = false;
        attemptFailed(QStringLiteral("no answer within %1 s").arg(kAnswerTimeoutMs / 1000));
    });
    m_openTimer.setSingleShot(true);
    connect(&m_openTimer, &QTimer::timeout, this, [this] {
        const int timeout = m_role == Role::Caller ? kCallerOpenTimeoutMs : kOpenTimeoutMs;
        const QString reason = QStringLiteral("data channel not open %1 s after the answer").arg(timeout / 1000);
        if (m_role == Role::Caller)
            attemptFailed(reason);
        else
            stop(QStringLiteral("Incoming call from %1 failed: %2").arg(m_peer, reason));
    });
    connect(m_signaling, &Signaling::message, this, &Connector::onSignal);
    connect(m_signaling, &Signaling::log, this, &Connector::log);
}

void Connector::setNodeName(const QString &name)
{
    if (m_state != State::Idle)
        disconnectPeer();
    m_name = name;
    emit log(QStringLiteral("Node %1: listening on topic %2 on %3 ntfy servers; %4 STUN servers in the list")
                 .arg(name, nodename::topic(name))
                 .arg(m_signaling->servers().size())
                 .arg(m_stun.size()));
    m_signaling->listen(nodename::topic(name));
}

void Connector::connectTo(const QString &peer)
{
    if (m_signaling->servers().isEmpty()) {
        emit log(QStringLiteral("ERROR: no ntfy servers configured"));
        return;
    }
    if (m_state != State::Idle)
        disconnectPeer();
    m_peer = peer;
    m_attemptsLeft = int(m_signaling->servers().size());
    setState(State::Connecting);
    startAttempt();
}

void Connector::disconnectPeer()
{
    if (m_state == State::Idle)
        return;
    // Tells a peer that is still setting up; an open channel closes by itself.
    m_signaling->publish(m_server, nodename::topic(m_peer), envelope("bye"), [](const QString &) {});
    stop(QStringLiteral("Disconnected from %1").arg(m_peer));
}

bool Connector::send(const QString &text)
{
    return m_state == State::Connected && m_link && m_link->send(text);
}

void Connector::startAttempt()
{
    m_role = Role::Caller;
    m_probing = false;
    m_sid = randomSid();
    m_server = m_nextServer % int(m_signaling->servers().size());
    m_stunTries = 0;
    m_stunClock.start();
    m_waitingForAnswer = false;
    m_clock.start();
    const int total = int(m_signaling->servers().size());
    const int attempt = total - m_attemptsLeft; // 0-based
    // Some home routers break hole punching when the peer's packet arrives
    // before the inside host has sent anything to it; a port-restricted NAT
    // on the other side needs that side to send before it lets anything in.
    // Which side must go first depends on the two networks, so it alternates.
    m_hold = attempt % 2 == 1;
    emit log(QStringLiteral("Calling %1, attempt %2/%3 via ntfy %4; %5")
                 .arg(m_peer)
                 .arg(attempt + 1)
                 .arg(total)
                 .arg(m_signaling->servers().at(m_server),
                      m_hold ? QStringLiteral("we send first (the answerer holds back %1 s)").arg(holdMs() / 1000.0)
                             : QStringLiteral("the answerer sends first")));
    startLink();
}

void Connector::startLink(const QString &stunServer)
{
    dropLink();
    const QString stun = stunServer.isEmpty() ? m_stun.pick() : stunServer;
    if (stunServer.isEmpty())
        ++m_stunTries;
    trace(QStringLiteral("STUN server %1%2").arg(stun, m_probing ? QStringLiteral(" (probe)") : QString()));
    m_link = new PeerLink(stun, kGatherTimeoutMs, this);
    connect(m_link, &PeerLink::localReady, this, &Connector::onLocalReady);
    connect(m_link, &PeerLink::opened, this, &Connector::onOpened);
    connect(m_link, &PeerLink::closed, this, &Connector::onClosed);
    connect(m_link, &PeerLink::message, this, &Connector::received);
    connect(m_link, &PeerLink::log, this, &Connector::trace);
    if (m_role == Role::Caller || m_probing)
        m_link->createOffer();
    else
        m_link->acceptOffer(m_remoteOffer, m_hold);
}

void Connector::dropLink()
{
    if (!m_link)
        return;
    m_link->disconnect(this);
    m_link->deleteLater();
    m_link = nullptr;
}

void Connector::onLocalReady(const QString &sdp, bool hasPublicAddress)
{
    // The answerer's real connection always answers; ICE may even have
    // connected on a LAN pair already, which ends gathering early in browsers.
    const bool mayRetry = m_role == Role::Caller || m_probing;
    if (!hasPublicAddress && mayRetry) {
        m_stun.markFailed(m_link->stunServer());
        const qint64 spent = m_link->elapsed();
        if (m_stunTries < kMaxStunTries && m_stunClock.elapsed() < kStunBudgetMs) {
            trace(QStringLiteral("no public address from %1 (%2), trying another STUN server")
                      .arg(m_link->stunServer(),
                           spent < kGatherTimeoutMs * 9 / 10 ? QStringLiteral("failed after %1 ms").arg(spent)
                                                    : QStringLiteral("no reply within %1 s").arg(kGatherTimeoutMs / 1000)));
            startLink();
            return;
        }
        trace(QStringLiteral("no public address from %1 STUN servers in %2 ms; host candidates only (LAN)")
                  .arg(m_stunTries)
                  .arg(m_stunClock.elapsed()));
    }
    if (m_probing) {
        m_probing = false;
        startLink(m_link->stunServer()); // the real connection, with the server that worked
        return;
    }

    const bool caller = m_role == Role::Caller;
    const QString what = caller ? QStringLiteral("offer") : QStringLiteral("answer");
    const QByteArray body = envelope(caller ? "offer" : "answer", sdp, caller && m_hold);
    trace(QStringLiteral("%1 ready (%2), %3 bytes").arg(what, candidateSummary(sdp)).arg(body.size()));
    if (body.size() > kMaxBodyBytes)
        trace(QStringLiteral("warning: over %1 bytes, ntfy may turn it into an attachment").arg(kMaxBodyBytes));

    if (caller) {
        m_waitingForAnswer = true;
        m_answerTimer.start(kAnswerTimeoutMs);
    } else {
        m_openTimer.start(kOpenTimeoutMs);
    }
    const QString sid = m_sid;
    const bool hold = !caller && m_hold;
    const QString server = m_signaling->servers().at(m_server);
    if (hold)
        trace(QStringLiteral("holding the caller's candidates until %1 ms after the answer is posted (caller sends first)")
                  .arg(holdMs()));
    QPointer<PeerLink> link = m_link;
    m_signaling->publish(m_server, nodename::topic(m_peer), body, [this, sid, server, what, caller, hold, link](const QString &error) {
        if (sid != m_sid || m_state != State::Connecting)
            return; // a newer attempt or call has taken over
        if (hold && link) {
            QTimer::singleShot(holdMs(), link, [this, link] {
                trace(QStringLiteral("applying the caller's candidates: our checks start now"));
                link->releaseRemoteCandidates();
            });
        }
        if (error.isEmpty())
            trace(QStringLiteral("%1 posted to ntfy %2").arg(what, server));
        else if (caller)
            attemptFailed(QStringLiteral("posting the offer to %1 failed: %2").arg(server, error));
        else
            trace(QStringLiteral("posting the answer to %1 failed: %2").arg(server, error));
    });
}

void Connector::onSignal(int server, const QByteArray &body)
{
    const QJsonObject msg = QJsonDocument::fromJson(body).object();
    if (msg.value(QLatin1String("v")).toInt() != 1)
        return; // not ours
    const QString type = msg.value(QLatin1String("type")).toString();
    const QString from = nodename::normalize(msg.value(QLatin1String("from")).toString());
    const QString sid = msg.value(QLatin1String("sid")).toString();
    const QString sdp = msg.value(QLatin1String("sdp")).toString();
    const bool hold = msg.value(QLatin1String("hold")).toBool();
    if (!nodename::isValid(from) || from == m_name || sid.isEmpty())
        return;

    if (type == QLatin1String("offer") && !sdp.isEmpty()) {
        onOffer(server, from, sid, sdp, hold);
    } else if (type == QLatin1String("answer") && !sdp.isEmpty()) {
        onAnswer(from, sid, sdp);
    } else if (type == QLatin1String("bye")) {
        if (m_state != State::Idle && from == m_peer && sid == m_sid)
            stop(QStringLiteral("%1 hung up").arg(from));
    }
}

void Connector::onOffer(int server, const QString &from, const QString &sid, const QString &sdp, bool hold)
{
    if (m_state != State::Idle && from == m_peer && sid == m_sid)
        return; // already handling this one
    if (m_state == State::Connecting && m_role == Role::Caller && from == m_peer) {
        // Both pressed Connect: the smaller name keeps its offer.
        if (m_name < from) {
            trace(QStringLiteral("%1 is calling us at the same time; keeping our offer (smaller name)").arg(from));
            return;
        }
        trace(QStringLiteral("%1 is calling us at the same time; answering theirs (larger name)").arg(from));
    } else if (m_state != State::Idle) {
        emit log(QStringLiteral("Incoming call from %1 replaces the %2 with %3")
                     .arg(from, m_state == State::Connected ? QStringLiteral("connection") : QStringLiteral("call"), m_peer));
    }

    dropLink();
    m_answerTimer.stop();
    m_openTimer.stop();
    m_waitingForAnswer = false;
    m_role = Role::Answerer;
    m_peer = from;
    m_sid = sid;
    m_server = server;
    m_remoteOffer = sdp;
    m_hold = hold;
    m_probing = true;
    m_stunTries = 0;
    m_stunClock.start();
    m_clock.start();
    emit peerChanged(from);
    setState(State::Connecting);
    emit log(QStringLiteral("Incoming call from %1 via ntfy %2 (offer: %3); %4")
                 .arg(from, m_signaling->servers().at(server), candidateSummary(sdp),
                      hold ? QStringLiteral("the caller sends first") : QStringLiteral("we send first")));
    startLink();
}

void Connector::onAnswer(const QString &from, const QString &sid, const QString &sdp)
{
    if (m_role != Role::Caller || !m_waitingForAnswer || from != m_peer || sid != m_sid) {
        trace(QStringLiteral("ignoring a stale answer from %1").arg(from));
        return;
    }
    m_waitingForAnswer = false;
    m_answerTimer.stop();
    trace(QStringLiteral("answer received (%1)").arg(candidateSummary(sdp)));
    m_openTimer.start(kCallerOpenTimeoutMs);
    m_link->setAnswer(sdp);
}

void Connector::onOpened()
{
    m_answerTimer.stop();
    m_openTimer.stop();
    if (m_role == Role::Caller)
        m_nextServer = m_server; // this one works; start here next time
    emit log(QStringLiteral("Connected to %1 after %2 ms (ntfy %3, STUN %4)")
                 .arg(m_peer)
                 .arg(m_clock.elapsed())
                 .arg(m_signaling->servers().at(m_server), m_link->stunServer()));
    setState(State::Connected);
}

void Connector::onClosed(const QString &reason)
{
    if (m_probing && m_stunTries < kMaxStunTries) {
        trace(QStringLiteral("probe failed (%1), trying another STUN server").arg(reason));
        startLink();
        return;
    }
    if (m_state == State::Connected)
        stop(QStringLiteral("Connection to %1 closed: %2").arg(m_peer, reason));
    else if (m_role == Role::Caller)
        attemptFailed(reason);
    else
        stop(QStringLiteral("Incoming call from %1 failed: %2").arg(m_peer, reason));
}

void Connector::attemptFailed(const QString &reason)
{
    if (m_state != State::Connecting || m_role != Role::Caller)
        return;
    trace(QStringLiteral("attempt failed: ") + reason);
    dropLink();
    m_answerTimer.stop();
    m_openTimer.stop();
    const int servers = int(m_signaling->servers().size());
    m_nextServer = (m_server + 1) % servers;
    if (--m_attemptsLeft > 0) {
        startAttempt();
        return;
    }
    stop(QStringLiteral("Giving up on %1 after trying all %2 ntfy servers; Connect tries again, starting at %3")
             .arg(m_peer)
             .arg(servers)
             .arg(m_signaling->servers().at(m_nextServer)));
}

void Connector::stop(const QString &reason)
{
    dropLink();
    m_answerTimer.stop();
    m_openTimer.stop();
    m_waitingForAnswer = false;
    m_probing = false;
    m_sid.clear();
    if (!reason.isEmpty())
        emit log(reason);
    setState(State::Idle);
}

void Connector::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

void Connector::trace(const QString &line)
{
    if (m_clock.isValid())
        emit log(QStringLiteral("  [+%1 ms] %2").arg(m_clock.elapsed(), 5).arg(line));
    else
        emit log(line);
}

QByteArray Connector::envelope(const char *type, const QString &sdp, bool hold) const
{
    QJsonObject msg{
        {QStringLiteral("v"), 1},
        {QStringLiteral("type"), QLatin1String(type)},
        {QStringLiteral("from"), m_name},
        {QStringLiteral("sid"), m_sid},
    };
    if (!sdp.isEmpty())
        msg.insert(QStringLiteral("sdp"), sdp);
    if (hold)
        msg.insert(QStringLiteral("hold"), true);
    return QJsonDocument(msg).toJson(QJsonDocument::Compact);
}
