#include "shimsocket.h"

#include "netshim.h"

#include <QPointer>

#include <atomic>
#include <cerrno>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// One wake-up for all shim objects. netshim calls notify() with its lock held,
// from any thread, so it only flips a flag and posts to the Qt thread.
class ShimHub : public QObject
{
public:
    static ShimHub *instance()
    {
        static ShimHub *hub = new ShimHub; // first use is on the Qt thread
        s_hub.store(hub);
        return hub;
    }

    static void notify(int, void *)
    {
        if (ShimHub *hub = s_hub.load())
            hub->schedule();
    }

    void schedule()
    {
        if (!m_scheduled.exchange(true))
            QMetaObject::invokeMethod(this, &ShimHub::serviceAll, Qt::QueuedConnection);
    }

    void add(ShimSocket *s) { m_sockets.append(s); }
    void add(ShimServer *s) { m_servers.append(s); }
    void remove(ShimSocket *s) { m_sockets.removeOne(s); }
    void remove(ShimServer *s) { m_servers.removeOne(s); }

private:
    void serviceAll()
    {
        m_scheduled.store(false);
        // Signals emitted while servicing may create or delete shim objects.
        for (ShimServer *server : QList<ShimServer *>(m_servers)) {
            if (m_servers.contains(server))
                server->service();
        }
        for (ShimSocket *socket : QList<ShimSocket *>(m_sockets)) {
            if (m_sockets.contains(socket))
                socket->service();
        }
    }

    static inline std::atomic<ShimHub *> s_hub{nullptr};
    std::atomic<bool> m_scheduled{false};
    QList<ShimSocket *> m_sockets;
    QList<ShimServer *> m_servers;
};

namespace {

sockaddr_in loopback(quint16 port)
{
    sockaddr_in sin = {};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return sin;
}

bool isLoopbackHost(const QString &host)
{
    return host == QLatin1String("localhost") || QHostAddress(host).isLoopback();
}

QAbstractSocket::SocketError socketErrorFromErrno(int err)
{
    switch (err) {
    case ECONNREFUSED: return QAbstractSocket::ConnectionRefusedError;
    case ENETUNREACH: return QAbstractSocket::NetworkError;
    case EMFILE: return QAbstractSocket::SocketResourceError;
    default: return QAbstractSocket::UnknownSocketError;
    }
}

// RFC 1928 reply codes, as tor's SOCKS port uses them.
QString socksReplyText(int code)
{
    switch (code) {
    case 1: return QStringLiteral("general SOCKS server failure");
    case 2: return QStringLiteral("connection not allowed by ruleset");
    case 3: return QStringLiteral("network unreachable");
    case 4: return QStringLiteral("host unreachable");
    case 5: return QStringLiteral("connection refused");
    case 6: return QStringLiteral("TTL expired");
    case 7: return QStringLiteral("command not supported");
    case 8: return QStringLiteral("address type not supported");
    default: return QStringLiteral("reply code %1").arg(code);
    }
}

} // namespace

// --- ShimSocket ----------------------------------------------------------------

ShimSocket::ShimSocket(QObject *parent) : QIODevice(parent)
{
    ShimHub::instance()->add(this);
}

ShimSocket::~ShimSocket()
{
    ShimHub::instance()->remove(this);
    if (m_fd >= 0) {
        netshim_set_notify(m_fd, nullptr, nullptr);
        ::close(m_fd);
    }
}

bool ShimSocket::attach(int fd)
{
    if (!netshim_is_shim_fd(fd))
        return false;
    m_fd = fd;
    m_rx.clear();
    m_tx.clear();
    m_closing = false;
    m_error = QAbstractSocket::UnknownSocketError;
    QIODevice::open(QIODevice::ReadWrite | QIODevice::Unbuffered);
    netshim_set_notify(fd, &ShimHub::notify, nullptr);
    ShimHub::instance()->schedule(); // data may already be waiting
    return true;
}

bool ShimSocket::setSocketDescriptor(qintptr fd)
{
    if (m_fd >= 0 || !attach(int(fd)))
        return false;
    m_state = QAbstractSocket::ConnectedState;
    return true;
}

void ShimSocket::connectToHost(const QString &host, quint16 port)
{
    if (m_state != QAbstractSocket::UnconnectedState)
        return;
    const bool socks = m_proxy.type() == QNetworkProxy::Socks5Proxy;
    const QString dialHost = socks ? m_proxy.hostName() : host;
    const quint16 dialPort = socks ? m_proxy.port() : port;
    if (!isLoopbackHost(dialHost)) {
        failLater(QAbstractSocket::NetworkError, QStringLiteral("only 127.0.0.1 is reachable in the browser"));
        return;
    }
    if (socks && host.toUtf8().size() > 255) {
        failLater(QAbstractSocket::HostNotFoundError, QStringLiteral("host name too long for SOCKS5"));
        return;
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        failLater(socketErrorFromErrno(errno), QString::fromLocal8Bit(strerror(errno)));
        return;
    }
    const sockaddr_in sin = loopback(dialPort);
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&sin), sizeof sin) < 0) {
        const int err = errno;
        ::close(fd);
        failLater(socketErrorFromErrno(err), QString::fromLocal8Bit(strerror(err)));
        return;
    }
    attach(fd);

    if (!socks) {
        m_state = QAbstractSocket::ConnectedState;
        QMetaObject::invokeMethod(this, &ShimSocket::connected, Qt::QueuedConnection);
        return;
    }
    // SOCKS5: no authentication, then CONNECT by host name.
    m_state = QAbstractSocket::ConnectingState;
    m_socks = Socks::Greeting;
    m_socksTarget = host.toUtf8();
    m_socksPort = port;
    ::send(m_fd, "\x05\x01\x00", 3, MSG_DONTWAIT); // a fresh loopback pipe always has room
}

void ShimSocket::disconnectFromHost()
{
    if (m_fd < 0)
        return;
    if (m_state == QAbstractSocket::ConnectedState && !m_tx.isEmpty()) {
        m_closing = true;
        m_state = QAbstractSocket::ClosingState;
        ShimHub::instance()->schedule();
        return;
    }
    QPointer<ShimSocket> self(this);
    finishClose(true);
    if (self)
        QIODevice::close();
}

void ShimSocket::abort()
{
    m_tx.clear();
    m_rx.clear();
    QPointer<ShimSocket> self(this);
    if (m_fd >= 0)
        finishClose(true);
    if (self)
        QIODevice::close();
}

qint64 ShimSocket::bytesAvailable() const
{
    return m_rx.size() + QIODevice::bytesAvailable();
}

qint64 ShimSocket::readData(char *data, qint64 maxSize)
{
    const qint64 n = qMin(maxSize, qint64(m_rx.size()));
    memcpy(data, m_rx.constData(), size_t(n));
    m_rx.remove(0, n);
    return n;
}

qint64 ShimSocket::writeData(const char *data, qint64 size)
{
    if (m_fd < 0 || m_closing) {
        setErrorString(QStringLiteral("socket is not connected"));
        return -1;
    }
    m_tx.append(data, size);
    ShimHub::instance()->schedule(); // written asynchronously, like QTcpSocket
    return size;
}

void ShimSocket::service()
{
    if (m_fd < 0)
        return;
    QPointer<ShimSocket> self(this);

    const qsizetype before = m_rx.size();
    bool eof = false;
    char buf[16384];
    for (;;) {
        const ssize_t n = ::recv(m_fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) {
            m_rx.append(buf, n);
            continue;
        }
        eof = n == 0 || errno != EAGAIN;
        break;
    }
    bool gotData = m_rx.size() > before;

    if (m_socks != Socks::None) {
        socksStep();
        if (!self || m_fd < 0)
            return;
        if (m_socks != Socks::None) {
            if (eof) {
                finishClose(false);
                m_error = QAbstractSocket::ProxyConnectionClosedError;
                setErrorString(QStringLiteral("SOCKS5 proxy closed the connection"));
                emit errorOccurred(m_error);
            }
            return;
        }
        gotData = !m_rx.isEmpty(); // bytes that followed the SOCKS reply
    }

    flush();
    if (!self || m_fd < 0)
        return;
    if (gotData && !m_rx.isEmpty()) {
        emit readyRead();
        if (!self || m_fd < 0)
            return;
    }
    if (eof)
        finishClose(true); // the remote end closed; m_rx stays readable
}

void ShimSocket::socksStep()
{
    if (m_socks == Socks::Greeting) {
        if (m_rx.size() < 2)
            return;
        if (m_rx.at(0) != 5 || m_rx.at(1) != 0) {
            finishClose(false);
            m_error = QAbstractSocket::ProxyProtocolError;
            setErrorString(QStringLiteral("SOCKS5 proxy refused the greeting"));
            emit errorOccurred(m_error);
            return;
        }
        m_rx.remove(0, 2);
        QByteArray request("\x05\x01\x00\x03", 4);
        request += char(m_socksTarget.size());
        request += m_socksTarget;
        request += char(m_socksPort >> 8);
        request += char(m_socksPort & 0xff);
        ::send(m_fd, request.constData(), size_t(request.size()), MSG_DONTWAIT);
        m_socks = Socks::Reply;
    }

    // VER REP RSV ATYP BND.ADDR BND.PORT
    if (m_rx.size() < 5)
        return;
    int addrLen = 0;
    switch (quint8(m_rx.at(3))) {
    case 1: addrLen = 4; break;
    case 4: addrLen = 16; break;
    case 3: addrLen = 1 + quint8(m_rx.at(4)); break;
    default: addrLen = -1; break;
    }
    const int reply = quint8(m_rx.at(1));
    if (addrLen < 0 || reply != 0) {
        finishClose(false);
        switch (reply) {
        case 4: m_error = QAbstractSocket::HostNotFoundError; break;
        case 5: m_error = QAbstractSocket::ConnectionRefusedError; break;
        case 6: m_error = QAbstractSocket::SocketTimeoutError; break;
        default: m_error = QAbstractSocket::ProxyProtocolError; break;
        }
        setErrorString(QStringLiteral("SOCKS5 proxy: ")
                       + (addrLen < 0 ? QStringLiteral("bad reply") : socksReplyText(reply)));
        emit errorOccurred(m_error);
        return;
    }
    if (m_rx.size() < 4 + addrLen + 2)
        return;
    m_rx.remove(0, 4 + addrLen + 2);
    m_socks = Socks::None;
    m_state = QAbstractSocket::ConnectedState;
    emit connected();
}

void ShimSocket::flush()
{
    if (m_state != QAbstractSocket::ConnectedState && m_state != QAbstractSocket::ClosingState)
        return;
    QPointer<ShimSocket> self(this);
    qint64 written = 0;
    while (!m_tx.isEmpty()) {
        const ssize_t n = ::send(m_fd, m_tx.constData(), size_t(m_tx.size()), MSG_DONTWAIT);
        if (n > 0) {
            m_tx.remove(0, n);
            written += n;
            continue;
        }
        if (n < 0 && errno != EAGAIN)
            m_tx.clear(); // EPIPE: the reader is gone; recv() will report EOF
        break;
    }
    if (written > 0) {
        emit bytesWritten(written);
        if (!self)
            return;
    }
    if (m_closing && m_tx.isEmpty() && m_fd >= 0) {
        finishClose(true);
        if (self)
            QIODevice::close();
    }
}

void ShimSocket::finishClose(bool emitDisconnected)
{
    const bool wasConnected =
        m_state == QAbstractSocket::ConnectedState || m_state == QAbstractSocket::ClosingState;
    if (m_fd >= 0) {
        netshim_set_notify(m_fd, nullptr, nullptr);
        ::close(m_fd);
        m_fd = -1;
    }
    m_state = QAbstractSocket::UnconnectedState;
    m_socks = Socks::None;
    m_closing = false;
    m_tx.clear();
    if (emitDisconnected && wasConnected)
        emit disconnected();
}

void ShimSocket::failLater(QAbstractSocket::SocketError error, const QString &reason)
{
    m_state = QAbstractSocket::UnconnectedState;
    m_error = error;
    setErrorString(reason);
    QMetaObject::invokeMethod(this, [this, error] { emit errorOccurred(error); }, Qt::QueuedConnection);
}

// --- ShimServer ----------------------------------------------------------------

ShimServer::ShimServer(QObject *parent) : QObject(parent)
{
    ShimHub::instance()->add(this);
}

ShimServer::~ShimServer()
{
    ShimHub::instance()->remove(this);
    close();
}

bool ShimServer::listen(const QHostAddress &address, quint16 port)
{
    if (m_fd >= 0 || !(address.isLoopback() || address == QHostAddress::Any || address == QHostAddress::AnyIPv4))
        return false;
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return false;
    sockaddr_in sin = loopback(port);
    socklen_t len = sizeof sin;
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&sin), sizeof sin) < 0 || ::listen(fd, 16) < 0
        || ::getsockname(fd, reinterpret_cast<sockaddr *>(&sin), &len) < 0) {
        ::close(fd);
        return false;
    }
    m_fd = fd;
    m_port = ntohs(sin.sin_port);
    netshim_set_notify(fd, &ShimHub::notify, nullptr);
    return true;
}

void ShimServer::close()
{
    if (m_fd < 0)
        return;
    netshim_set_notify(m_fd, nullptr, nullptr);
    ::close(m_fd);
    m_fd = -1;
    m_port = 0;
}

ShimSocket *ShimServer::nextPendingConnection()
{
    return m_pending.isEmpty() ? nullptr : m_pending.takeFirst();
}

void ShimServer::service()
{
    if (m_fd < 0)
        return;
    bool added = false;
    for (;;) {
        const int fd = ::accept4(m_fd, nullptr, nullptr, SOCK_NONBLOCK);
        if (fd < 0)
            break;
        auto *socket = new ShimSocket(this);
        socket->setSocketDescriptor(fd);
        m_pending.append(socket);
        added = true;
    }
    if (added)
        emit newConnection();
}
