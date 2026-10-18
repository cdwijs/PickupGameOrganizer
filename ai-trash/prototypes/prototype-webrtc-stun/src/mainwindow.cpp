#include "mainwindow.h"

#include "connector.h"
#include "nodename.h"

#include <QCheckBox>
#include <QDebug>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputMethod>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#ifndef Q_OS_WASM
#include <rtc/rtc.hpp>
#endif

namespace {

// Two instances side by side need different names: WEBRTC_STUN_PROFILE
// picks a separate settings group.
QString settingsKey(const char *name = "nodeName")
{
    QString profile = qEnvironmentVariable("WEBRTC_STUN_PROFILE");
    if (profile.isEmpty())
        profile = QStringLiteral("default");
    return QStringLiteral("profile-%1/%2").arg(profile, QLatin1String(name));
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_connector(new Connector(this)),
#ifdef Q_OS_WASM
      // The browser's localStorage, written synchronously.
      m_settings(new QSettings(QSettings::WebLocalStorageFormat, QSettings::UserScope,
                               QStringLiteral("PickupGameOrganizer"), QStringLiteral("prototype-webrtc-stun"), this)),
#else
      m_settings(new QSettings(QStringLiteral("PickupGameOrganizer"), QStringLiteral("prototype-webrtc-stun"), this)),
#endif
      m_nodeName(new QLineEdit),
      m_generateButton(new QPushButton(tr("Generate"))),
      m_peerName(new QLineEdit),
      m_connectButton(new QPushButton(tr("Connect"))),
      m_sendLine(new QLineEdit),
      m_sendButton(new QPushButton(tr("Send"))),
      m_receiveBox(new QPlainTextEdit),
      m_statusBox(new QPlainTextEdit),
      m_webRtcLog(new QCheckBox(tr("WebRTC debug log")))
{
    setWindowTitle(tr("WebRTC STUN prototype"));
    resize(760, 640);

    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_nodeName->setReadOnly(true);
    m_nodeName->setFont(mono);
    m_generateButton->setToolTip(tr("Generate a new node name (others reach you by it)"));
    m_peerName->setFont(mono);
    m_peerName->setPlaceholderText(tr("e.g. brave-otter-417"));
    // Android's keyboard composes text before committing it, and a composing
    // QLineEdit reports no text until the composition ends. Node names are
    // not words, so predictive text has nothing to offer here anyway.
    m_peerName->setInputMethodHints(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
    m_sendLine->setPlaceholderText(tr("Message"));
    // Chat keeps predictive text, so Send cannot wait for textChanged (which
    // stays silent while Android composes); it commits the composition
    // itself. Without focus, tapping it also keeps the keyboard open.
    m_sendButton->setFocusPolicy(Qt::NoFocus);
    m_receiveBox->setReadOnly(true);
    m_receiveBox->setMaximumBlockCount(5000);
    m_statusBox->setReadOnly(true);
    m_statusBox->setFont(mono);
    m_statusBox->setMaximumBlockCount(20000);
    m_webRtcLog->setToolTip(tr("libdatachannel/libjuice details: every ICE check sent and received. "
                               "Applies to connections set up after ticking it."));

    // Labels sit above their fields, so on a phone the fields get the full width.
    auto *column = new QVBoxLayout;
    auto addLabel = [column](const QString &text) {
        column->addSpacing(4);
        column->addWidget(new QLabel(text));
    };
    auto addRow = [column](QWidget *field, QPushButton *button) {
        auto *row = new QHBoxLayout;
        row->addWidget(field, 1);
        row->addWidget(button);
        column->addLayout(row);
    };
    addLabel(tr("Node name"));
    addRow(m_nodeName, m_generateButton);
    addLabel(tr("Other node"));
    addRow(m_peerName, m_connectButton);
    addLabel(tr("Send"));
    addRow(m_sendLine, m_sendButton);
    addLabel(tr("Received"));
    column->addWidget(m_receiveBox, 2);
    column->addSpacing(4);
    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(new QLabel(tr("Status / debug")), 1);
    statusRow->addWidget(m_webRtcLog);
    column->addLayout(statusRow);
    column->addWidget(m_statusBox, 3);

    auto *central = new QWidget;
    central->setLayout(column);
    setCentralWidget(central);

    connect(m_connector, &Connector::log, this, &MainWindow::status);
    connect(m_connector, &Connector::stateChanged, this, &MainWindow::updateButtons);
    connect(m_connector, &Connector::peerChanged, m_peerName, &QLineEdit::setText);
    connect(m_connector, &Connector::received, this, [this](const QString &text) {
        qInfo().noquote() << "Received from" << m_connector->peerName() << ":" << text;
        appendChat(m_connector->peerName(), text);
    });

    connect(m_generateButton, &QPushButton::clicked, this, [this] {
        const QString name = nodename::generate();
        m_settings->setValue(settingsKey(), name);
        setNodeName(name);
    });
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::toggleConnection);
    connect(m_peerName, &QLineEdit::returnPressed, this, [this] {
        if (m_connector->state() == Connector::State::Idle)
            toggleConnection();
    });
    connect(m_sendButton, &QPushButton::clicked, this, &MainWindow::sendMessage);
#ifdef Q_OS_WASM
    m_webRtcLog->hide(); // the browser's own WebRTC: see chrome://webrtc-internals
#else
    m_webRtcLog->setChecked(m_settings->value(settingsKey("webRtcLog")).toBool()
                            || qEnvironmentVariableIsSet("WEBRTC_STUN_RTCLOG"));
    setWebRtcLog(m_webRtcLog->isChecked());
    connect(m_webRtcLog, &QCheckBox::toggled, this, [this](bool on) {
        m_settings->setValue(settingsKey("webRtcLog"), on);
        setWebRtcLog(on);
    });
#endif
    connect(m_sendLine, &QLineEdit::returnPressed, this, &MainWindow::sendMessage);

    // The node is reachable right away: reuse the saved name or make one.
    // WEBRTC_STUN_NAME overrides it without saving (headless tests).
    QString name = nodename::normalize(qEnvironmentVariable("WEBRTC_STUN_NAME"));
    if (!nodename::isValid(name)) {
        name = m_settings->value(settingsKey()).toString();
        if (!nodename::isValid(name)) {
            name = nodename::generate();
            m_settings->setValue(settingsKey(), name);
        }
    }
    setNodeName(name);
    updateButtons();

    // Headless runs: WEBRTC_STUN_CONNECT=<peer> calls it once our own
    // subscriptions had a moment to open; WEBRTC_STUN_AUTOSEND=<text> sends
    // one message whenever a connection opens.
    const QString autoConnect = nodename::normalize(qEnvironmentVariable("WEBRTC_STUN_CONNECT"));
    if (!autoConnect.isEmpty()) {
        m_peerName->setText(autoConnect);
        QTimer::singleShot(3000, this, &MainWindow::toggleConnection);
    }
    const QString autoSend = qEnvironmentVariable("WEBRTC_STUN_AUTOSEND");
    if (!autoSend.isEmpty()) {
        connect(m_connector, &Connector::stateChanged, this, [this, autoSend](Connector::State state) {
            if (state == Connector::State::Connected) {
                m_sendLine->setText(autoSend);
                sendMessage();
            }
        });
    }
}

MainWindow::~MainWindow()
{
#ifndef Q_OS_WASM
    rtc::InitLogger(rtc::LogLevel::Warning, nullptr); // no callbacks into a dead window
#endif
}

// libdatachannel's warnings and errors always go to the status pane; with
// `verbose`, so does libjuice's ICE detail (checks sent and received, pair
// states, consent). libjuice reads the level when a peer connection is
// created, so it applies to the next connection.
void MainWindow::setWebRtcLog(bool verbose)
{
#ifdef Q_OS_WASM
    Q_UNUSED(verbose);
#else
    rtc::InitLogger(verbose ? rtc::LogLevel::Verbose : rtc::LogLevel::Warning,
                    [this](rtc::LogLevel level, std::string message) {
        // libdatachannel's own verbose output (SCTP, DTLS internals) is noise
        // here; libjuice's debug lines arrive as verbose and are the point.
        QString text = QString::fromStdString(message);
        // "rtc::impl::IceTransport::LogCallback@391: juice: ..." -> "juice: ..."
        const qsizetype at = text.indexOf(QLatin1Char('@'));
        const qsizetype colon = text.indexOf(QLatin1String(": "));
        if (at >= 0 && colon > at)
            text.remove(0, colon + 2);
        // libjuice's debug lines (verbose here) are mostly per-packet noise
        // (STUN attribute encoding, the poll loop); keep the ICE events.
        static const QRegularExpression iceEvents(QStringLiteral(
            "^juice: (STUN entry \\d+: (Sending|Cancelled|Failed)|Received STUN (datagram from|Binding)"
            "|Adding (remote candidate|new candidate pair)|Obtained a new remote reflexive"
            "|C\\w*andidate pair check|Got a nominated|Pair nomination|Requesting pair nomination"
            "|New selected|Missing remote|Triggered pair check|Got STUN Binding error|Response has mapped"
            "|Gathered|Consent|Remote agent)"));
        if (level > rtc::LogLevel::Debug && !iceEvents.match(text).hasMatch())
            return;
        QMetaObject::invokeMethod(this, [this, text] { status(QStringLiteral("    rtc ") + text); },
                                  Qt::QueuedConnection);
    });
    if (verbose)
        status(QStringLiteral("WebRTC debug log on (applies to the next connection)"));
#endif
}

void MainWindow::status(const QString &line)
{
    qInfo().noquote() << line;
    m_statusBox->appendPlainText(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz ")) + line);
}

void MainWindow::setNodeName(const QString &name)
{
    m_nodeName->setText(name);
    m_connector->setNodeName(name);
}

void MainWindow::toggleConnection()
{
    if (m_connector->state() != Connector::State::Idle) {
        m_connector->disconnectPeer();
        return;
    }
    const QString peer = nodename::normalize(m_peerName->text());
    if (!nodename::isValid(peer)) {
        status(QStringLiteral("Enter the other node's name first (letters, digits and dashes)"));
        return;
    }
    if (peer == m_connector->nodeName()) {
        status(QStringLiteral("That is this node's own name"));
        return;
    }
    m_peerName->setText(peer);
    m_connector->connectTo(peer);
}

void MainWindow::sendMessage()
{
    QGuiApplication::inputMethod()->commit(); // the word still being composed on Android
    const QString text = m_sendLine->text();
    if (text.isEmpty())
        return;
    if (!m_connector->send(text)) {
        status(QStringLiteral("Not sent: no open connection"));
        return;
    }
    appendChat(QStringLiteral("me"), text);
    m_sendLine->clear();
}

void MainWindow::appendChat(const QString &who, const QString &text)
{
    m_receiveBox->appendPlainText(QStringLiteral("[%1] %2: %3")
                                      .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), who, text));
}

void MainWindow::updateButtons()
{
    const Connector::State state = m_connector->state();
    m_connectButton->setText(state == Connector::State::Idle ? tr("Connect") : tr("Disconnect"));
    m_peerName->setReadOnly(state != Connector::State::Idle);
    const bool connected = state == Connector::State::Connected;
    m_sendButton->setEnabled(connected);
    m_sendLine->setEnabled(connected);
}
