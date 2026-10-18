#pragma once

// One WebRTC peer connection with one ordered, reliable data channel "chat",
// using a single STUN server and no TURN.
//
// No trickle ICE: the local description goes out once, with its candidates
// inlined. It is ready as soon as a server-reflexive (srflx, i.e. public)
// candidate has arrived plus a short grace period for the rest, when
// gathering completes, or after the gathering timeout, whichever comes first.
//
// libdatachannel calls back on its own threads; every callback is re-posted
// to this object's thread, so the rest of the code is single-threaded Qt.

#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace rtc {
class DataChannel;
class PeerConnection;
} // namespace rtc

class PeerLink : public QObject
{
    Q_OBJECT

public:
    PeerLink(const QString &stunServer, int gatherTimeoutMs, QObject *parent = nullptr);
    ~PeerLink() override;

    const QString &stunServer() const { return m_stunServer; }
    bool isOpen() const { return m_open; }
    qint64 elapsed() const { return m_clock.elapsed(); } // since construction

    void createOffer();                      // -> localReady(offer)
    // holdCandidates: apply the offer without its candidates, so this side
    // sends no checks until releaseRemoteCandidates(); incoming checks are
    // still answered.
    void acceptOffer(const QString &offer, bool holdCandidates = false); // -> localReady(answer)
    void releaseRemoteCandidates();
    void setAnswer(const QString &answer);   // -> opened() or closed()
    bool send(const QString &text);

signals:
    void localReady(const QString &sdp, bool hasPublicAddress);
    void opened();
    void closed(const QString &reason);
    void message(const QString &text);
    void log(const QString &line);

private:
    void attachChannel(std::shared_ptr<rtc::DataChannel> channel);
    void addLocalCandidate(const QString &candidate);
    void finishGathering();
    void fail(const QString &reason);

    const QString m_stunServer;
    std::shared_ptr<rtc::PeerConnection> m_pc;
    std::shared_ptr<rtc::DataChannel> m_dc;
    QString m_localSdp;
    QStringList m_candidates;
    QStringList m_heldCandidates; // the offer's a=candidate lines, while held
    QString m_remoteMid;
    bool m_hasSrflx = false;
    bool m_wantFinish = false;
    bool m_ready = false;
    bool m_open = false;
    bool m_closed = false;
    QTimer m_graceTimer;
    QTimer m_gatherTimer;
    QElapsedTimer m_clock;
};

// "2 host, 1 srflx" style summary of the a=candidate lines in an SDP.
QString candidateSummary(const QString &sdp);
