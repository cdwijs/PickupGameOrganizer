#pragma once

// One WebRTC connection to a Snowflake proxy (client/lib/webrtc.go): create
// an offer with a single ordered data channel, wait for ICE gathering to
// finish (no trickle ICE), hand the offer to the broker, apply the answer.
//
// libdatachannel calls back on its own threads; every callback is re-posted
// to this object's thread, so the rest of the code is single-threaded Qt.

#include "peer.h"

#include <QStringList>

#include <memory>

namespace rtc {
class DataChannel;
class PeerConnection;
} // namespace rtc

namespace snowflake {

class WebRtcPeer : public Peer
{
    Q_OBJECT

public:
    WebRtcPeer(const QStringList &iceServers, const QString &label, QObject *parent = nullptr);
    ~WebRtcPeer() override;

    void createOffer();                       // -> offerReady()
    void setAnswer(const QString &answerSdp); // -> opened() or closed()
    bool send(const QByteArray &bytes) override;

signals:
    void offerReady(const QString &sdp);

private:
    std::shared_ptr<rtc::PeerConnection> m_pc;
    std::shared_ptr<rtc::DataChannel> m_dc;
    bool m_offerSent = false;
};

// Remove host candidates with private/loopback addresses, like util.StripLocalAddresses.
QString stripLocalCandidates(const QString &sdp);

// "2 host, 1 srflx" style summary of the a=candidate lines, for the debug log.
QString candidateSummary(const QString &sdp);

} // namespace snowflake
