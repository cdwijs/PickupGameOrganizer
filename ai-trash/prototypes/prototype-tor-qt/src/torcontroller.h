#pragma once

// Runs tor inside this process (libtor.a, tor_api.h) on its own thread and
// drives it over the owning control socket that tor_api hands us: a socketpair
// that is already authenticated, and whose closing makes tor exit.

#include "loopback.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class QTemporaryDir;
class QThread;

class TorController : public QObject
{
    Q_OBJECT

public:
    enum class Mode {
        DirectGuards, // EntryNodes = the pinned guards
        Snowflake,    // UseBridges via the in-process Snowflake client
    };

    explicit TorController(const QString &dataDir, QObject *parent = nullptr);
    ~TorController() override;

    // tor can only be started once per process.
    void start(Mode mode, const QStringList &guardFingerprints, quint16 snowflakePort);

    // Replace the current onion service (if any) with a freshly keyed v3 one
    // that forwards virtPort to 127.0.0.1:localPort.
    // Publishes the onion service with the key saved in the profile, so the
    // address survives restarts (and, in the browser, reloads). newAddress
    // throws the saved key away first.
    void createOnion(quint16 virtPort, quint16 localPort, bool newAddress = false);

    quint16 socksPort() const { return m_socksPort; }

signals:
    void log(const QString &line);
    void controlReady();
    void bootstrapProgress(int percent, const QString &summary);
    void socksReady(quint16 port);
    void onionCreated(const QString &serviceId);
    void onionPublished(const QString &serviceId);
    void failed(const QString &reason);

private:
    using ReplyHandler = std::function<void(int code, const QList<QByteArray> &lines)>;

    bool useTemporaryDirIfLocked();
    QString onionKeyPath() const;
    void saveOnionKey(const QByteArray &key);
    bool writeTorrc(Mode mode, const QStringList &guardFingerprints, quint16 snowflakePort);
    void onControlConnected();
    void sendCommand(const QByteArray &command, ReplyHandler handler);
    void onControlReadyRead();
    void handleReply(int code, const QList<QByteArray> &lines);
    void handleEvent(const QList<QByteArray> &lines);

    QString m_dataDir;
    const QString m_profileDir; // m_dataDir unless the temporary fallback is used
    std::unique_ptr<QTemporaryDir> m_tempDir; // when the profile dir is locked
    QThread *m_torThread = nullptr;
    std::atomic<int> m_exitCode{0}; // tor_run_main()'s return value
    bool m_stopping = false;
    std::vector<QByteArray> m_args; // must outlive tor_run_main
    std::vector<char *> m_argv;
    void *m_config = nullptr;       // tor_main_configuration_t

    ControlSocket m_control;
    QByteArray m_rxBuffer;
    QList<QByteArray> m_replyLines;
    bool m_inDataBlock = false;
    QQueue<ReplyHandler> m_pending;

    bool m_ready = false;
    quint16 m_socksPort = 0;
    QString m_serviceId;
    bool m_published = false;
};
