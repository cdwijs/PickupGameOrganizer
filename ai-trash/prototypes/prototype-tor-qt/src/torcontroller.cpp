#include "torcontroller.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>

#ifndef Q_OS_WASM
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

extern "C" {
#include <tor_api.h>
}

namespace {

// torrc values may contain spaces; quote them the way torrc expects.
QString torrcQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    escaped.replace(QLatin1Char('"'), QLatin1String("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

// Value of KEY=... inside an event/reply line, unquoted.
QString keywordValue(const QByteArray &line, const QByteArray &key)
{
    const QByteArray needle = key + '=';
    int start = line.indexOf(needle);
    if (start < 0)
        return {};
    start += needle.size();
    if (start < line.size() && line.at(start) == '"') {
        const int end = line.indexOf('"', start + 1);
        return QString::fromUtf8(line.mid(start + 1, end < 0 ? -1 : end - start - 1));
    }
    const int end = line.indexOf(' ', start);
    return QString::fromUtf8(line.mid(start, end < 0 ? -1 : end - start));
}

// Onion private keys (ADD_ONION ED25519-V3:<key>, PrivateKey=ED25519-V3:<key>)
// must not end up in the status pane or on stderr.
QString redactKeys(const QByteArray &line)
{
    static const QRegularExpression key(QStringLiteral("ED25519-V3:[A-Za-z0-9+/=]+"));
    return QString::fromUtf8(line).replace(key, QStringLiteral("ED25519-V3:<redacted>"));
}

// True while another tor holds <dataDir>/lock. tor takes it with
// flock(LOCK_EX | LOCK_NB) and, finding it held, waits 5 s and exits.
bool dataDirLocked(const QString &dataDir)
{
#ifdef Q_OS_WASM
    // One tab, one tor: there is no other process to share /persist with.
    Q_UNUSED(dataDir);
    return false;
#else
    const int fd = ::open(QFile::encodeName(dataDir + QStringLiteral("/lock")).constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return false; // no lock file: tor never ran here
    const bool locked = ::flock(fd, LOCK_EX | LOCK_NB) < 0 && errno == EWOULDBLOCK;
    ::close(fd); // also drops our lock, if we got it
    return locked;
#endif
}

// Temporary data dirs left behind by instances that crashed or were killed
// (a debugger stop skips the destructor). A dir still locked by tor, or only
// just created by an instance that is about to start tor, is kept.
int removeStaleTemporaryDirs()
{
    int removed = 0;
    const QDir temp(QDir::tempPath());
    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-60);
    for (const QFileInfo &info : temp.entryInfoList({QStringLiteral("prototype-tor-qt-*")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (info.lastModified() > cutoff || dataDirLocked(info.filePath()))
            continue;
        removed += QDir(info.filePath()).removeRecursively() ? 1 : 0;
    }
    return removed;
}

// Tor Browser's default Snowflake bridges (snowflake client/torrc, 2026-10).
// AMP cache / SQS rendezvous and uTLS are not implemented in the C++ client,
// so only url= + fronts= (domain fronting through CDN77) are used.
const char *const kSnowflakeBridges[] = {
    "snowflake 192.0.2.3:80 2B280B23E1107BB62ABFC40DDCC8824814F80A72 "
    "fingerprint=2B280B23E1107BB62ABFC40DDCC8824814F80A72 url=https://1098762253.rsc.cdn77.org/ "
    "fronts=www.cdn77.com,www.phpmyadmin.net "
    "ice=stun:stun.antisip.com:3478,stun:stun.epygi.com:3478,stun:stun.uls.co.za:3478,"
    "stun:stun.voipgate.com:3478,stun:stun.mixvoip.com:3478,stun:stun.nextcloud.com:3478,"
    "stun:stun.bethesda.net:3478,stun:stun.nextcloud.com:443",
    "snowflake 192.0.2.4:80 8838024498816A039FCBBAB14E6F40A0843051FA "
    "fingerprint=8838024498816A039FCBBAB14E6F40A0843051FA url=https://1098762253.rsc.cdn77.org/ "
    "fronts=www.cdn77.com,www.phpmyadmin.net "
    "ice=stun:stun.antisip.com:3478,stun:stun.epygi.com:3478,stun:stun.uls.co.za:3478,"
    "stun:stun.voipgate.com:3478,stun:stun.mixvoip.com:3478,stun:stun.nextcloud.com:3478,"
    "stun:stun.bethesda.net:3478,stun:stun.nextcloud.com:443",
};

} // namespace

TorController::TorController(const QString &dataDir, QObject *parent)
    : QObject(parent), m_dataDir(dataDir), m_profileDir(dataDir)
{
    connect(&m_control, &ControlSocket::readyRead, this, &TorController::onControlReadyRead);
    connect(&m_control, &ControlSocket::disconnected, this, [this] {
        emit failed(QStringLiteral("tor closed the control connection (tor stopped)"));
    });
}

TorController::~TorController()
{
    if (!m_torThread)
        return;
    // Closing the owning control socket makes tor shut down.
    m_stopping = true;
    m_control.disconnect(this);
    m_control.abort();
#ifdef Q_OS_WASM
    // The browser's main thread must not block, and tor's file I/O is proxied
    // to this very thread; the tab going away ends tor anyway.
    return;
#endif
    if (m_torThread->wait(5000)) {
        delete m_torThread;
        tor_main_configuration_free(static_cast<tor_main_configuration_t *>(m_config));
    }
    else {
        // Leak the thread, config and temporary data dir; the process is
        // exiting anyway and destroying a running QThread would abort.
        if (m_tempDir)
            m_tempDir->setAutoRemove(false);
    }
}

// Another instance (or a debug session that is still alive) runs tor on this
// profile. Run in a throwaway data dir instead, seeded with the profile's
// cached directory documents so bootstrap does not start from zero. tor
// checks their signatures, so copying them is safe.
bool TorController::useTemporaryDirIfLocked()
{
    if (const int removed = removeStaleTemporaryDirs())
        emit log(QStringLiteral("Removed %1 stale temporary data dir(s) from %2").arg(removed).arg(QDir::tempPath()));
    if (!dataDirLocked(m_dataDir))
        return true;

    m_tempDir = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/prototype-tor-qt-XXXXXX"));
    if (!m_tempDir->isValid()) {
        emit failed(QStringLiteral("%1 is in use by another instance, and no temporary data dir could be created: %2")
                        .arg(m_dataDir, m_tempDir->errorString()));
        return false;
    }
    const QDir profile(m_dataDir);
    const QStringList cached = profile.entryList({QStringLiteral("cached-*")}, QDir::Files);
    int copied = 0;
    for (const QString &name : cached)
        copied += QFile::copy(profile.filePath(name), m_tempDir->filePath(name)) ? 1 : 0;

    emit log(QStringLiteral("%1 is in use by another instance; using the temporary data dir %2 (%3 cached "
                            "directory files copied), removed on exit. Set TOR_QT_PROFILE to keep a separate "
                            "persistent profile.")
                 .arg(m_dataDir, m_tempDir->path())
                 .arg(copied));
    m_dataDir = m_tempDir->path();
    return true;
}

bool TorController::writeTorrc(Mode mode, const QStringList &guardFingerprints, quint16 snowflakePort)
{
    QStringList config = {
        QStringLiteral("DataDirectory ") + torrcQuote(m_dataDir),
        QStringLiteral("SocksPort auto"),
        QStringLiteral("ClientOnly 1"),
        QStringLiteral("AvoidDiskWrites 1"),
        // tor's log reaches the GUI through control-port events instead.
        QStringLiteral("Log err stderr"),
        // Leave SIGINT/SIGTERM to the app; main() ignores SIGPIPE in tor's place.
        QStringLiteral("__DisableSignalHandlers 1"),
    };
#ifdef Q_OS_WASM
    // Every tor thread is a Web Worker from a fixed pool; a client needs one cpuworker.
    config << QStringLiteral("NumCPUs 1");
#endif
    if (mode == Mode::DirectGuards) {
        QStringList entryNodes;
        for (const QString &fp : guardFingerprints)
            entryNodes << QLatin1Char('$') + fp;
        config << QStringLiteral("EntryNodes ") + entryNodes.join(QLatin1Char(','))
               << QStringLiteral("UseEntryGuards 1");
    } else {
        config << QStringLiteral("UseBridges 1")
               << QStringLiteral("ClientTransportPlugin snowflake socks5 127.0.0.1:%1").arg(snowflakePort);
        for (const char *bridge : kSnowflakeBridges)
            config << QStringLiteral("Bridge ") + QString::fromLatin1(bridge);
    }

    QFile torrc(m_dataDir + QStringLiteral("/torrc"));
    if (!torrc.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    torrc.write(config.join(QLatin1Char('\n')).toUtf8() + '\n');
    QFile defaults(m_dataDir + QStringLiteral("/torrc-defaults"));
    return defaults.open(QIODevice::WriteOnly | QIODevice::Truncate); // empty: ignore /etc/tor
}

void TorController::start(Mode mode, const QStringList &guardFingerprints, quint16 snowflakePort)
{
    if (m_torThread) {
        emit log(QStringLiteral("tor is already running (it can only start once per process)"));
        return;
    }
#ifdef Q_OS_WASM
    if (mode == Mode::DirectGuards) {
        emit failed(QStringLiteral("a browser cannot open TCP connections to guards; use Snowflake"));
        return;
    }
#endif

    // tor refuses a DataDirectory that other users can read.
    QDir().mkpath(m_dataDir);
    if (!useTemporaryDirIfLocked())
        return;
    QFile::setPermissions(m_dataDir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    if (!writeTorrc(mode, guardFingerprints, snowflakePort)) {
        emit failed(QStringLiteral("cannot write torrc in ") + m_dataDir);
        return;
    }

    m_args = {"tor", "-f", (m_dataDir + QStringLiteral("/torrc")).toUtf8(),
              "--defaults-torrc", (m_dataDir + QStringLiteral("/torrc-defaults")).toUtf8()};
    m_argv.clear();
    for (QByteArray &arg : m_args)
        m_argv.push_back(arg.data());
    m_argv.push_back(nullptr);

    auto *cfg = tor_main_configuration_new();
    m_config = cfg;
    tor_main_configuration_set_command_line(cfg, int(m_args.size()), m_argv.data());
    const tor_control_socket_t fd = tor_main_configuration_setup_control_socket(cfg);
    if (fd == INVALID_TOR_CONTROL_SOCKET) {
        emit failed(QStringLiteral("tor_api: could not create the control socket"));
        return;
    }

    emit log(QStringLiteral("Starting embedded %1 (%2), data dir %3")
                 .arg(QString::fromLatin1(tor_api_get_provider_version()),
                      mode == Mode::DirectGuards ? QStringLiteral("%1 pinned entry guards").arg(guardFingerprints.size())
                                                 : QStringLiteral("Snowflake bridges"),
                      m_dataDir));

    m_torThread = QThread::create([this, cfg] { m_exitCode = tor_run_main(cfg); });
    m_torThread->setObjectName(QStringLiteral("tor"));
    connect(m_torThread, &QThread::finished, this, [this] {
        emit log(QStringLiteral("tor thread finished (exit code %1)").arg(m_exitCode.load()));
        if (!m_stopping) {
            m_ready = false;
            emit failed(QStringLiteral("tor stopped on its own (exit code %1); its [err] lines are on stderr. "
                                       "Restart the app to start tor again.")
                            .arg(m_exitCode.load()));
        }
    });
    m_torThread->start();

    m_control.setSocketDescriptor(fd);
    onControlConnected();
}

void TorController::onControlConnected()
{
    // The owning control connection is pre-authenticated (CC_LOCAL_FD_IS_AUTHENTICATED).
    m_ready = true;
    sendCommand("SETEVENTS NOTICE WARN ERR STATUS_CLIENT HS_DESC", [](int, const QList<QByteArray> &) {});

    sendCommand("GETINFO net/listeners/socks", [this](int code, const QList<QByteArray> &lines) {
        if (code != 250 || lines.isEmpty())
            return;
        // net/listeners/socks="127.0.0.1:41235"
        const QString value = keywordValue(lines.first(), "net/listeners/socks");
        m_socksPort = value.section(QLatin1Char(':'), -1).toUShort();
        emit log(QStringLiteral("SOCKS port is %1").arg(m_socksPort));
        emit socksReady(m_socksPort);
    });

    sendCommand("GETINFO status/bootstrap-phase", [this](int code, const QList<QByteArray> &lines) {
        if (code == 250 && !lines.isEmpty())
            emit bootstrapProgress(keywordValue(lines.first(), "PROGRESS").toInt(),
                                   keywordValue(lines.first(), "SUMMARY"));
    });

    emit controlReady();
}

// Empty while running on the temporary fallback: the instance that holds the
// profile may be publishing the same onion, so this one gets a throwaway key.
QString TorController::onionKeyPath() const
{
    return m_tempDir ? QString() : m_profileDir + QStringLiteral("/onion-key");
}

void TorController::saveOnionKey(const QByteArray &key)
{
    QSaveFile file(onionKeyPath());
    if (!file.open(QIODevice::WriteOnly) || file.write(key + '\n') < 0 || !file.commit()) {
        emit log(QStringLiteral("Could not save the onion key to %1: %2").arg(file.fileName(), file.errorString()));
        return;
    }
    QFile::setPermissions(file.fileName(), QFile::ReadOwner | QFile::WriteOwner);
    emit log(QStringLiteral("Onion key saved to %1; the address stays the same after a restart").arg(file.fileName()));
}

void TorController::createOnion(quint16 virtPort, quint16 localPort, bool newAddress)
{
    if (!m_ready) {
        emit log(QStringLiteral("Tor is not running yet"));
        return;
    }
    if (!m_serviceId.isEmpty()) {
        emit log(QStringLiteral("Removing old onion service %1").arg(m_serviceId));
        sendCommand("DEL_ONION " + m_serviceId.toLatin1(), [](int, const QList<QByteArray> &) {});
        m_serviceId.clear();
    }

    const QString keyPath = onionKeyPath();
    QByteArray key;
    if (keyPath.isEmpty()) {
        emit log(QStringLiteral("Temporary data dir: this onion address is new and not saved"));
    } else if (newAddress) {
        QFile::remove(keyPath);
    } else {
        QFile file(keyPath);
        if (file.open(QIODevice::ReadOnly))
            key = file.readAll().trimmed();
        if (!key.isEmpty() && !key.startsWith("ED25519-V3:")) {
            emit log(QStringLiteral("Ignoring %1: not an ED25519-V3 key").arg(keyPath));
            key.clear();
        }
    }
    const bool saveKey = key.isEmpty() && !keyPath.isEmpty();

    const QByteArray ports = QStringLiteral("Port=%1,127.0.0.1:%2").arg(virtPort).arg(localPort).toLatin1();
    QByteArray cmd = "ADD_ONION NEW:ED25519-V3 " + QByteArray(saveKey ? "" : "Flags=DiscardPK ") + ports;
    if (!key.isEmpty())
        cmd = "ADD_ONION " + key + ' ' + ports;
    sendCommand(cmd, [this, saveKey](int code, const QList<QByteArray> &lines) {
        if (code != 250) {
            emit log(QStringLiteral("ADD_ONION failed: ") + QString::fromUtf8(lines.value(0)));
            return;
        }
        for (const QByteArray &line : lines) {
            if (line.startsWith("PrivateKey=") && saveKey)
                saveOnionKey(line.mid(11));
            if (line.startsWith("ServiceID=")) {
                m_serviceId = QString::fromLatin1(line.mid(10));
                m_published = false;
                emit onionCreated(m_serviceId);
            }
        }
    });
}

void TorController::sendCommand(const QByteArray &command, ReplyHandler handler)
{
    emit log(QStringLiteral("> ") + redactKeys(command));
    m_pending.enqueue(std::move(handler));
    m_control.write(command + "\r\n");
}

void TorController::onControlReadyRead()
{
    m_rxBuffer += m_control.readAll();
    int eol;
    while ((eol = m_rxBuffer.indexOf("\r\n")) >= 0) {
        const QByteArray line = m_rxBuffer.left(eol);
        m_rxBuffer.remove(0, eol + 2);

        if (m_inDataBlock) {
            if (line == ".")
                m_inDataBlock = false;
            else
                m_replyLines << line;
            continue;
        }
        if (line.size() < 4)
            continue;

        // "CODE-" mid reply, "CODE+" data block follows, "CODE " last line.
        const int code = line.left(3).toInt();
        const char sep = line.at(3);
        m_replyLines << line.mid(4);
        if (sep == '+') {
            m_inDataBlock = true;
        } else if (sep == ' ') {
            const QList<QByteArray> lines = std::exchange(m_replyLines, {});
            if (code >= 600 && code < 700)
                handleEvent(lines);
            else
                handleReply(code, lines);
        }
    }
}

void TorController::handleReply(int code, const QList<QByteArray> &lines)
{
    for (const QByteArray &line : lines)
        emit log(QStringLiteral("< %1 ").arg(code) + redactKeys(line));
    if (m_pending.isEmpty())
        return;
    ReplyHandler handler = m_pending.dequeue();
    if (handler)
        handler(code, lines);
}

void TorController::handleEvent(const QList<QByteArray> &lines)
{
    const QByteArray &line = lines.first();

    // NOTICE / WARN / ERR <message>: tor's own log
    for (const char *severity : {"NOTICE ", "WARN ", "ERR "}) {
        if (line.startsWith(severity)) {
            emit log(QStringLiteral("[tor] ") + QString::fromUtf8(line));
            return;
        }
    }

    // STATUS_CLIENT NOTICE BOOTSTRAP PROGRESS=45 TAG=... SUMMARY="..."
    if (line.startsWith("STATUS_CLIENT")) {
        if (line.contains(" BOOTSTRAP "))
            emit bootstrapProgress(keywordValue(line, "PROGRESS").toInt(), keywordValue(line, "SUMMARY"));
        return;
    }

    // HS_DESC UPLOADED <address> UNKNOWN <hsdir> ...
    if (line.startsWith("HS_DESC ")) {
        const QList<QByteArray> parts = line.split(' ');
        const QString action = QString::fromLatin1(parts.value(1));
        const QString address = QString::fromLatin1(parts.value(2));
        if (address != m_serviceId)
            return;
        emit log(QStringLiteral("Descriptor %1 (hsdir %2)").arg(action, QString::fromLatin1(parts.value(4))));
        if (action == QLatin1String("UPLOADED") && !m_published) {
            m_published = true;
            emit onionPublished(m_serviceId);
        }
        return;
    }

    emit log(QStringLiteral("< 650 ") + QString::fromUtf8(line));
}
