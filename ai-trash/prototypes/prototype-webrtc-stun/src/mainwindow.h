#pragma once

#include <QMainWindow>

class Connector;
class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSettings;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void status(const QString &line);
    void setNodeName(const QString &name);
    void toggleConnection();
    void sendMessage();
    void appendChat(const QString &who, const QString &text);
    void updateButtons();
    void setWebRtcLog(bool verbose);

    Connector *m_connector;
    QSettings *m_settings;

    QLineEdit *m_nodeName;
    QPushButton *m_generateButton;
    QLineEdit *m_peerName;
    QPushButton *m_connectButton;
    QLineEdit *m_sendLine;
    QPushButton *m_sendButton;
    QPlainTextEdit *m_receiveBox;
    QPlainTextEdit *m_statusBox;
    QCheckBox *m_webRtcLog;
};
