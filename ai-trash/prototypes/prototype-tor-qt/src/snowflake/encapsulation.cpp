#include "encapsulation.h"

namespace snowflake {

QByteArray encapsulate(const QByteArray &packet)
{
    const int n = packet.size();
    QByteArray out;
    out.reserve(n + 3);
    if ((n >> 6) == 0) {
        out.append(char(0x80 | n));
    } else if ((n >> 13) == 0) {
        out.append(char(0xc0 | ((n >> 7) & 0x3f)));
        out.append(char(n & 0x7f));
    } else {
        // 20-bit limit; KCP packets are ~1400 bytes, far below it.
        out.append(char(0xc0 | ((n >> 14) & 0x3f)));
        out.append(char(0x80 | ((n >> 7) & 0x7f)));
        out.append(char(n & 0x7f));
    }
    out.append(packet);
    return out;
}

bool EncapsulationDecoder::next(QByteArray *packet, bool *error)
{
    *error = false;
    for (;;) {
        if (m_buffer.isEmpty())
            return false;

        const auto first = quint8(m_buffer.at(0));
        const bool isData = first & 0x80;
        bool more = first & 0x40;
        int length = first & 0x3f;
        int prefix = 1;
        while (more) {
            if (prefix >= 3) {
                *error = true;
                return false;
            }
            if (m_buffer.size() <= prefix)
                return false;
            const auto b = quint8(m_buffer.at(prefix++));
            more = b & 0x80;
            length = (length << 7) | (b & 0x7f);
        }
        if (m_buffer.size() < prefix + length)
            return false;

        if (isData) {
            *packet = m_buffer.mid(prefix, length);
            m_buffer.remove(0, prefix + length);
            return true;
        }
        m_buffer.remove(0, prefix + length); // padding
    }
}

} // namespace snowflake
