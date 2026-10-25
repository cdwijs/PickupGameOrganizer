#include "core.h"

#include <QTest>

class TstCore : public QObject
{
    Q_OBJECT

private slots:
    void greetingIsNotEmpty()
    {
        QVERIFY(!greeting().isEmpty());
    }
};

QTEST_MAIN(TstCore)
#include "tst_core.moc"
