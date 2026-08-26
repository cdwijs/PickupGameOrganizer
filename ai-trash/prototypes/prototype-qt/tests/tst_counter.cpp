// Drives the real widget with synthetic clicks under the offscreen platform,
// so "the textbox counts the button presses" is checked rather than assumed.

#include "counterwindow.h"

#include <QLineEdit>
#include <QPushButton>
#include <QTest>

class TestCounter : public QObject
{
    Q_OBJECT

private slots:
    void startsAtZero();
    void countsClicks();
    void displayIsReadOnly();
};

void TestCounter::startsAtZero()
{
    CounterWindow window;
    QCOMPARE(window.findChild<QLineEdit *>("display")->text(), QStringLiteral("0"));
    QCOMPARE(window.presses(), 0);
}

void TestCounter::countsClicks()
{
    CounterWindow window;
    auto *button = window.findChild<QPushButton *>("press");
    auto *display = window.findChild<QLineEdit *>("display");

    for (int expected = 1; expected <= 3; ++expected) {
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(display->text(), QString::number(expected));
        QCOMPARE(window.presses(), expected);
    }
}

void TestCounter::displayIsReadOnly()
{
    CounterWindow window;
    auto *display = window.findChild<QLineEdit *>("display");
    QVERIFY(display->isReadOnly());

    // Typing into it must not turn the count into something the button did not
    // produce.
    QTest::keyClicks(display, QStringLiteral("99"));
    QCOMPARE(display->text(), QStringLiteral("0"));
}

QTEST_MAIN(TestCounter)
#include "tst_counter.moc"
