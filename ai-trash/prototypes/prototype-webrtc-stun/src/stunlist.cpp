#include "stunlist.h"

#include <QFile>
#include <QRandomGenerator>

StunList::StunList()
    : m_first(qEnvironmentVariable("WEBRTC_STUN_FIRST"))
{
    QFile file(QStringLiteral(":/resources/stun-servers.txt"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (const QByteArray &line : file.readAll().split('\n')) {
            const QString server = QString::fromUtf8(line).trimmed();
            if (!server.isEmpty())
                m_servers << server;
        }
    }
}

QString StunList::pick()
{
    if (!m_first.isEmpty())
        return std::exchange(m_first, QString());
    if (m_servers.isEmpty())
        return {};
    if (m_failed.size() >= m_servers.size())
        m_failed.clear(); // all failed once: give every server another chance
    QStringList candidates;
    for (const QString &server : std::as_const(m_servers)) {
        if (!m_failed.contains(server))
            candidates << server;
    }
    return candidates.at(QRandomGenerator::global()->bounded(candidates.size()));
}

void StunList::markFailed(const QString &server)
{
    m_failed.insert(server);
}
