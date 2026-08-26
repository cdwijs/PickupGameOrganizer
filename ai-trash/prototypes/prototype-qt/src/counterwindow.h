// The whole application: one button, one read-only textbox counting its
// presses. In a header so the test can drive the same widget the three
// packaged builds ship.

#pragma once

#include <QFont>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

// A plain widget rather than a QMainWindow: there is no menu bar, tool bar or
// status bar to hang off one, and on Android and WebAssembly the window is the
// whole screen either way.
class CounterWindow : public QWidget
{
    Q_OBJECT

public:
    CounterWindow()
    {
        setWindowTitle(tr("Qt counter"));

        auto *caption = new QLabel(tr("Button presses"), this);

        m_display = new QLineEdit(this);
        m_display->setObjectName(QStringLiteral("display"));
        m_display->setReadOnly(true);           // a display, not an input field
        m_display->setAlignment(Qt::AlignCenter);
        m_display->setFocusPolicy(Qt::NoFocus); // never raises the phone keyboard
        QFont big = m_display->font();
        big.setPointSize(big.pointSize() * 3);
        m_display->setFont(big);

        m_button = new QPushButton(tr("Press me"), this);
        m_button->setObjectName(QStringLiteral("press"));
        m_button->setMinimumHeight(48);         // a comfortable touch target
        connect(m_button, &QPushButton::clicked, this, &CounterWindow::countPress);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(24, 24, 24, 24);
        layout->setSpacing(16);
        layout->addStretch();
        layout->addWidget(caption, 0, Qt::AlignHCenter);
        layout->addWidget(m_display);
        layout->addWidget(m_button);
        layout->addStretch();

        showCount();
        resize(360, 260);
    }

    int presses() const { return m_presses; }

private:
    void countPress()
    {
        ++m_presses;
        showCount();
    }

    void showCount() { m_display->setText(QString::number(m_presses)); }

    QLineEdit *m_display = nullptr;
    QPushButton *m_button = nullptr;
    int m_presses = 0;
};
