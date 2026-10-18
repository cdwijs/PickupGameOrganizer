#pragma once

// Sets up one chat connection at a time, by node name.
//
// Calling: the offer goes to the peer's topic on one ntfy server. The first
// attempt uses the first server in the list; each failed attempt (POST
// error, no answer in 15 s, channel not open 8 s after the answer) moves on
// to the next server, once around the list. The next Connect continues where
// the rotation stopped.
//
// Who sends the first connectivity check alternates per attempt: on odd
// attempts the answerer starts as soon as it has the offer; on even ones the
// offer carries "hold" and the answerer waits until 2 s after posting its
// answer, so the caller goes first. Some home routers break hole punching
// when the peer's packet arrives before the inside host has sent anything
// (seen 2026-10-18 with a home router vs. a mobile carrier NAT).
//
// Answering: every offer is accepted (the newest wins) and answered on the
// server it arrived on, which the caller is listening on.
//
// Each side picks a random STUN server; one that yields no public address
// within 2 s is replaced by another, for up to 8 s and 10 servers, before
// falling back to host candidates only (LAN). The caller simply replaces its
// peer connection. The answerer must not: a connection that has seen the
// offer starts ICE checks at once, and the caller would lock onto one that
// is then thrown away. So the answerer first probes STUN servers with a
// throwaway connection that never sees the offer, then creates the one real
// connection with the server that worked and always sends its answer.

#include "stunlist.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

class PeerLink;
class Signaling;

class Connector : public QObject
{
    Q_OBJECT

public:
    enum class State { Idle, Connecting, Connected };
    Q_ENUM(State)

    explicit Connector(QObject *parent = nullptr);

    State state() const { return m_state; }
    const QString &nodeName() const { return m_name; }
    const QString &peerName() const { return m_peer; }

    void setNodeName(const QString &name); // listens on its topic on every server
    void connectTo(const QString &peer);
    void disconnectPeer();
    bool send(const QString &text);

signals:
    void stateChanged(Connector::State state);
    void peerChanged(const QString &peer);
    void received(const QString &text);
    void log(const QString &line);

private:
    enum class Role { Caller, Answerer };

    void startAttempt();
    void startLink(const QString &stunServer = QString());
    void dropLink();
    void onLocalReady(const QString &sdp, bool hasPublicAddress);
    void onSignal(int server, const QByteArray &body);
    void onOffer(int server, const QString &from, const QString &sid, const QString &sdp, bool hold);
    void onAnswer(const QString &from, const QString &sid, const QString &sdp);
    void onOpened();
    void onClosed(const QString &reason);
    void attemptFailed(const QString &reason);
    void stop(const QString &reason);
    void setState(State state);
    void trace(const QString &line); // log with ms since the attempt started
    QByteArray envelope(const char *type, const QString &sdp = QString(), bool hold = false) const;

    Signaling *m_signaling;
    StunList m_stun;
    PeerLink *m_link = nullptr;
    QString m_name;
    QString m_peer;
    QString m_sid;
    QString m_remoteOffer;
    Role m_role = Role::Caller;
    State m_state = State::Idle;
    int m_server = 0;      // ntfy server of the current attempt
    int m_nextServer = 0;  // where the next call starts
    int m_attemptsLeft = 0;
    int m_stunTries = 0;
    bool m_waitingForAnswer = false;
    bool m_probing = false; // answerer: looking for a working STUN server
    bool m_hold = false;    // this attempt: the answerer holds back, the caller sends first
    QTimer m_answerTimer;
    QTimer m_openTimer;
    QElapsedTimer m_clock;
    QElapsedTimer m_stunClock; // since the first STUN server of this attempt
};
