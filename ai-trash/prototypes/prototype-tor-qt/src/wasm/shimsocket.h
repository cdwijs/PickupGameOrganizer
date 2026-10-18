#pragma once

// Qt stand-ins for QTcpSocket, QTcpServer and QLocalSocket on netshim fds
// (WebAssembly only; see loopback.h). Only the API subset that TorController,
// ChatLink and snowflake::Client use is provided, with the same names and
// signals, so the shared code compiles against either.
//
// netshim's readiness callback can fire on any thread (usually tor's). It
// only posts one wake-up to the Qt thread, which then services every shim
// object with non-blocking calls: the browser's main thread must not block.

#include <QAbstractSocket>
#include <QByteArray>
#include <QHostAddress>
#include <QIODevice>
#include <QList>
#include <QNetworkProxy>
#include <QObject>
#include <QString>

class ShimSocket : public QIODevice
{
    Q_OBJECT

public:
    explicit ShimSocket(QObject *parent = nullptr);
    ~ShimSocket() override;

    // Adopt an already connected shim fd (tor_api's control socket, accept()).
    bool setSocketDescriptor(qintptr fd);

    // Only a SOCKS5 proxy on 127.0.0.1 is supported (tor's SOCKS port); the
    // host name is resolved by the proxy, which is what .onion needs.
    void setProxy(const QNetworkProxy &proxy) { m_proxy = proxy; }

    // Without a proxy only 127.0.0.1 / localhost can be reached.
    void connectToHost(const QString &host, quint16 port);
    void disconnectFromHost();
    void abort();

    QAbstractSocket::SocketState state() const { return m_state; }
    QAbstractSocket::SocketError error() const { return m_error; }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;
    qint64 bytesToWrite() const override { return m_tx.size(); }
    void close() override { disconnectFromHost(); }

signals:
    void connected();
    void disconnected();
    void errorOccurred(QAbstractSocket::SocketError error);

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 size) override;

private:
    friend class ShimHub;
    enum class Socks { None, Greeting, Reply };

    void service();
    bool attach(int fd);
    void flush();
    void socksStep();
    void finishClose(bool emitDisconnected);
    void failLater(QAbstractSocket::SocketError error, const QString &reason);

    int m_fd = -1;
    QAbstractSocket::SocketState m_state = QAbstractSocket::UnconnectedState;
    QAbstractSocket::SocketError m_error = QAbstractSocket::UnknownSocketError;
    QNetworkProxy m_proxy;
    QByteArray m_rx;
    QByteArray m_tx;
    bool m_closing = false; // disconnectFromHost() waits for m_tx to drain

    Socks m_socks = Socks::None;
    QByteArray m_socksTarget; // host name for the CONNECT request
    quint16 m_socksPort = 0;
};

class ShimServer : public QObject
{
    Q_OBJECT

public:
    explicit ShimServer(QObject *parent = nullptr);
    ~ShimServer() override;

    bool listen(const QHostAddress &address, quint16 port = 0);
    bool isListening() const { return m_fd >= 0; }
    quint16 serverPort() const { return m_port; }
    void close();

    // Accepted sockets are children of the server, as with QTcpServer.
    ShimSocket *nextPendingConnection();

signals:
    void newConnection();

private:
    friend class ShimHub;
    void service();

    int m_fd = -1;
    quint16 m_port = 0;
    QList<ShimSocket *> m_pending;
};
