#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>

// Reports when the app goes to the background and comes back: the page's
// visibilitychange in the browser (iOS suspends a hidden tab within seconds),
// the application state elsewhere (Android).
class AppVisibility : public QObject
{
    Q_OBJECT
public:
    explicit AppVisibility(QObject *parent = nullptr);
    ~AppVisibility() override;

signals:
    void hidden();
    // How long we were away, by the wall clock and by the monotonic clock
    // that tor's and Snowflake's timers run on. They differ when the device
    // slept; tor then assumes its circuits are dead.
    void shown(qint64 wallMs, qint64 monotonicMs);

private:
    void setHidden(bool isHidden);

    bool m_hidden = false;
    qint64 m_hiddenAtWall = 0;
    QElapsedTimer m_hiddenAt;
};
