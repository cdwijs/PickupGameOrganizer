#pragma once

#include <QMainWindow>

class AppVisibility;
class ChatLink;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class TorController;

namespace snowflake {
class Client;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    void status(const QString &line);
    void sendMessage();
    void startTor();

    TorController *m_tor;
    ChatLink *m_chat;
    snowflake::Client *m_snowflake;
    AppVisibility *m_visibility;

    QComboBox *m_mode;
    QPushButton *m_startButton;

    QPushButton *m_generateButton;
    QPushButton *m_newAddressButton;
    QLineEdit *m_onionAddress;
    QLineEdit *m_peerAddress;
    QPlainTextEdit *m_sendBox;
    QPushButton *m_sendButton;
    QPlainTextEdit *m_receiveBox;
    QPlainTextEdit *m_statusBox;
};
