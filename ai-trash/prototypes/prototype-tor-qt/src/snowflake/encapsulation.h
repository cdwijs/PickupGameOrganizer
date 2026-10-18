#pragma once

// Snowflake's packet encapsulation (common/encapsulation in the Go source):
// KCP packets travel over the WebRTC data channel as a byte stream of
// length-prefixed records. The prefix is 1-3 bytes:
//   first byte  1 D M nnnnnn   D=1 data, D=0 padding; M=1 more length bytes follow
//   next bytes  M nnnnnnn      (at most two)

#include <QByteArray>

namespace snowflake {

// Prefix + packet, ready to write to the data channel.
QByteArray encapsulate(const QByteArray &packet);

// Incremental decoder: data channel messages need not align with records.
class EncapsulationDecoder
{
public:
    void feed(const QByteArray &bytes) { m_buffer += bytes; }
    // Next complete data record, skipping padding. False when more input is needed;
    // sets *error on a malformed prefix.
    bool next(QByteArray *packet, bool *error);
    void clear() { m_buffer.clear(); }

private:
    QByteArray m_buffer;
};

} // namespace snowflake
