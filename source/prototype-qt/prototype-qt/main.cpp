#include "mainwindow.h"

#include <QApplication>
#include <QLocale>
#include <QTranslator>

#include "groupadministration.h"

GroupAdministration *myGroupAdministration;

int main(int argc, char *argv[])
{
    qputenv("QT_FORCE_STDERR_LOGGING", "1"); //force debug messages to show up in 3 Application Output
    QApplication a(argc, argv);

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = "prototype-qt_" + QLocale(locale).name();
        if (translator.load(":/i18n/" + baseName)) {
            a.installTranslator(&translator);
            break;
        }
    }
//    MainWindow w;
//    w.show();

    myGroupAdministration = new GroupAdministration();

    return QApplication::exec();
}
