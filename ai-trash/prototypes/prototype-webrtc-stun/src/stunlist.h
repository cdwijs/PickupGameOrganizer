#pragma once

// The STUN servers from mondain's gist (resources/stun-servers.txt, copied
// verbatim). About half of them no longer answer, so servers that gave no
// public address are skipped for the rest of the session.

#include <QSet>
#include <QStringList>

class StunList
{
public:
    StunList();

    int size() const { return m_servers.size(); }

    // A random "host:port" that has not failed yet. WEBRTC_STUN_FIRST, if set,
    // is returned on the first call (for testing the dead-server retry).
    QString pick();
    void markFailed(const QString &server);

private:
    QStringList m_servers;
    QSet<QString> m_failed;
    QString m_first;
};
