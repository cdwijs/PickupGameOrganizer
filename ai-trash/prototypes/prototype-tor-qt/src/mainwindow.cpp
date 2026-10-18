#include "mainwindow.h"

#include "appvisibility.h"
#include "chatlink.h"
#include "snowflake/client.h"
#include "torcontroller.h"

#include <QComboBox>
#include <QDebug>
#include <QFile>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTime>

namespace {

// First 10 guards from the bundled guard-nodes.json snapshot.
QStringList loadGuardFingerprints(QStringList *descriptions)
{
    QFile file(QStringLiteral(":/resources/guard-nodes.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QStringList fingerprints;
    const QJsonArray nodes = QJsonDocument::fromJson(file.readAll()).array();
    for (const QJsonValue &v : nodes) {
        const QJsonObject node = v.toObject();
        fingerprints << node.value(QLatin1String("fingerprint")).toString();
        *descriptions << QStringLiteral("%1 %2 (%3)").arg(node.value(QLatin1String("nickname")).toString(),
                                                          node.value(QLatin1String("address")).toString(),
                                                          node.value(QLatin1String("country")).toString());
        if (fingerprints.size() == 10)
            break;
    }
    return fingerprints;
}

// One tor data dir per profile, so two instances can run side by side.
QString dataDirectory()
{
    QString profile = qEnvironmentVariable("TOR_QT_PROFILE");
    if (profile.isEmpty())
        profile = QStringLiteral("default");
#ifdef Q_OS_WASM
    // web/index.html mounts IndexedDB here, so the consensus survives reloads.
    return QStringLiteral("/persist/tor-") + profile;
#else
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/tor-")
           + profile;
#endif
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_tor(new TorController(dataDirectory(), this)),
      m_chat(new ChatLink(this)),
      m_snowflake(new snowflake::Client(this)),
      m_visibility(new AppVisibility(this)),
      m_mode(new QComboBox),
      m_startButton(new QPushButton(tr("Start Tor"))),
      m_generateButton(new QPushButton(tr("Start onion service"))),
      m_newAddressButton(new QPushButton(tr("New address"))),
      m_onionAddress(new QLineEdit),
      m_peerAddress(new QLineEdit),
      m_sendBox(new QPlainTextEdit),
      m_sendButton(new QPushButton(tr("Send"))),
      m_receiveBox(new QPlainTextEdit),
      m_statusBox(new QPlainTextEdit)
{
    setWindowTitle(tr("Tor node prototype"));
    resize(820, 720);

    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_onionAddress->setReadOnly(true);
    m_onionAddress->setFont(mono);
    m_onionAddress->setPlaceholderText(tr("(no onion service yet)"));
    m_peerAddress->setFont(mono);
    m_peerAddress->setPlaceholderText(tr("peer onion address, e.g. abc…xyz.onion"));
    m_sendBox->setPlaceholderText(tr("Message to send (Ctrl+Enter or the Send button)"));
    m_receiveBox->setReadOnly(true);
    m_statusBox->setReadOnly(true);
    m_statusBox->setFont(mono);
    m_statusBox->setMaximumBlockCount(5000);
    m_generateButton->setEnabled(false);
    m_generateButton->setToolTip(tr("Publish the onion service; reuses the address saved in this profile"));
    m_newAddressButton->setEnabled(false);
    m_newAddressButton->setToolTip(tr("Throw the saved key away and publish under a new address"));
    m_sendButton->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));

    m_mode->addItem(tr("Direct: 10 pinned guards"), int(TorController::Mode::DirectGuards));
    m_mode->addItem(tr("Snowflake (WebRTC proxies)"), int(TorController::Mode::Snowflake));
#ifdef Q_OS_WASM
    // A browser cannot open TCP connections, so the guards are out of reach.
    m_mode->setItemText(0, tr("Direct: not possible in a browser"));
    if (auto *model = qobject_cast<QStandardItemModel *>(m_mode->model()))
        model->item(0)->setEnabled(false);
    m_mode->setItemText(1, tr("Snowflake (WebRTC proxies, WebSocket fallback)"));
    m_mode->setCurrentIndex(1);
#endif

    auto *grid = new QGridLayout;
    grid->addWidget(m_startButton, 0, 0);
    grid->addWidget(m_mode, 0, 1);
    grid->addWidget(m_generateButton, 1, 0);
    auto *onionRow = new QHBoxLayout;
    onionRow->addWidget(m_onionAddress, 1);
    onionRow->addWidget(m_newAddressButton);
    grid->addLayout(onionRow, 1, 1);
    grid->addWidget(new QLabel(tr("Peer:")), 2, 0);
    grid->addWidget(m_peerAddress, 2, 1);
    grid->addWidget(new QLabel(tr("Send:")), 3, 0, Qt::AlignTop);
    grid->addWidget(m_sendBox, 3, 1);
    grid->addWidget(m_sendButton, 4, 1, Qt::AlignRight);
    grid->addWidget(new QLabel(tr("Received:")), 5, 0, Qt::AlignTop);
    grid->addWidget(m_receiveBox, 5, 1);
    grid->addWidget(new QLabel(tr("Status / debug:")), 6, 0, Qt::AlignTop);
    grid->addWidget(m_statusBox, 6, 1);
    grid->setRowStretch(3, 1);
    grid->setRowStretch(5, 2);
    grid->setRowStretch(6, 3);

    auto *central = new QWidget;
    central->setLayout(grid);
    setCentralWidget(central);

    connect(m_tor, &TorController::log, this, &MainWindow::status);
    connect(m_tor, &TorController::failed, this, [this](const QString &reason) {
        status(QStringLiteral("ERROR: ") + reason);
    });
    connect(m_tor, &TorController::controlReady, this, [this] {
        m_generateButton->setEnabled(true);
        m_newAddressButton->setEnabled(true);
    });
    connect(m_tor, &TorController::socksReady, m_chat, &ChatLink::setSocksPort);
    connect(m_tor, &TorController::bootstrapProgress, this, [this](int percent, const QString &summary) {
        status(QStringLiteral("Bootstrap %1%: %2").arg(percent).arg(summary));
    });
    connect(m_tor, &TorController::onionCreated, this, [this](const QString &serviceId) {
        m_onionAddress->setText(serviceId + QStringLiteral(".onion"));
        status(QStringLiteral("Onion service created; publishing descriptor (can take a minute)"));
    });
    connect(m_tor, &TorController::onionPublished, this, [this](const QString &serviceId) {
        status(QStringLiteral("Onion service %1.onion is published and reachable").arg(serviceId));
    });

    connect(m_snowflake, &snowflake::Client::log, this, &MainWindow::status);
    connect(m_chat, &ChatLink::log, this, &MainWindow::status);
    connect(m_chat, &ChatLink::received, this, [this](const QString &from, const QString &text) {
        qInfo().noquote() << "Received from" << from << ":" << text;
        m_receiveBox->appendPlainText(QStringLiteral("[%1] %2: %3")
                                          .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), from, text));
    });

    connect(m_generateButton, &QPushButton::clicked, this, [this] {
        m_tor->createOnion(ChatLink::kVirtualPort, m_chat->listen());
    });
    connect(m_newAddressButton, &QPushButton::clicked, this, [this] {
        m_tor->createOnion(ChatLink::kVirtualPort, m_chat->listen(), true);
    });

    // Timestamps for measuring what a trip to the background costs (iOS
    // suspends a hidden tab), and an immediate proxy check on return.
    connect(m_visibility, &AppVisibility::hidden, this, [this] { status(QStringLiteral("App hidden")); });
    connect(m_visibility, &AppVisibility::shown, this, [this](qint64 wallMs, qint64 monotonicMs) {
        status(QStringLiteral("App visible again after %1 s (wall clock), %2 s (monotonic clock)")
                   .arg(wallMs / 1000.0, 0, 'f', 1)
                   .arg(monotonicMs / 1000.0, 0, 'f', 1));
        // tor's second_elapsed_callback(): a jump of 100 s or more whose
        // clocks disagree by more than 2 s makes it drop its circuits.
        // An hour or more counts as idle and has the same effect.
        if (wallMs >= 100000 && qAbs(wallMs - monotonicMs) > 2000)
            status(QStringLiteral("The clocks disagree: tor will treat this as a clock jump and rebuild its circuits"));
        else if (wallMs >= 3600000)
            status(QStringLiteral("Away for an hour or more: tor will assume its circuits are dead and rebuild them"));
        m_snowflake->checkPeers();
    });
    connect(m_sendButton, &QPushButton::clicked, this, &MainWindow::sendMessage);
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::startTor);

    status(tr("Choose how to reach the Tor network, then press Start Tor."));

    // Headless runs: TOR_QT_AUTOSTART=direct|snowflake presses Start Tor, and
    // the onion service is generated as soon as tor has bootstrapped.
    // TOR_QT_AUTOSEND=<peer onion> then sends it one message once our own
    // service is published.
    const QString autostart = qEnvironmentVariable("TOR_QT_AUTOSTART");
    if (!autostart.isEmpty()) {
#ifndef Q_OS_WASM
        m_mode->setCurrentIndex(autostart == QLatin1String("snowflake") ? 1 : 0);
#endif
        connect(m_tor, &TorController::bootstrapProgress, this, [this](int percent, const QString &) {
            if (percent == 100 && m_onionAddress->text().isEmpty())
                m_generateButton->click();
        });
        QMetaObject::invokeMethod(this, &MainWindow::startTor, Qt::QueuedConnection);
    }
    const QString autosend = qEnvironmentVariable("TOR_QT_AUTOSEND");
    if (!autosend.isEmpty()) {
        connect(m_tor, &TorController::onionPublished, this, [this, autosend](const QString &serviceId) {
            m_peerAddress->setText(autosend);
            m_sendBox->setPlainText(QStringLiteral("hello from %1.onion").arg(serviceId));
            sendMessage();
        }, Qt::SingleShotConnection);
    }
}

void MainWindow::startTor()
{
    m_startButton->setEnabled(false);
    m_mode->setEnabled(false);
    const auto mode = TorController::Mode(m_mode->currentData().toInt());

    QStringList guards;
    quint16 snowflakePort = 0;
    if (mode == TorController::Mode::DirectGuards) {
        QStringList guardDescriptions;
        guards = loadGuardFingerprints(&guardDescriptions);
        status(QStringLiteral("Using %1 entry guards:").arg(guards.size()));
        for (const QString &g : guardDescriptions)
            status(QStringLiteral("  ") + g);
    } else {
        snowflakePort = m_snowflake->listen();
        status(QStringLiteral("Snowflake client listening on 127.0.0.1:%1").arg(snowflakePort));
    }
    m_tor->start(mode, guards, snowflakePort);
}

void MainWindow::status(const QString &line)
{
    qInfo().noquote() << line; // stderr on Linux, logcat on Android
    m_statusBox->appendPlainText(QTime::currentTime().toString(QStringLiteral("HH:mm:ss ")) + line);
}

void MainWindow::sendMessage()
{
    const QString text = m_sendBox->toPlainText();
    if (text.isEmpty())
        return;
    m_chat->send(m_peerAddress->text(), text);
    m_sendBox->clear();
}
