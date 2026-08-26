// prototype-qt — one button, one textbox counting the presses.
//
// Deliberately the whole application: no resources, no .ui file, no QML.
// Everything that differs between the three targets lives in build.sh and
// CMakeLists.txt, so this source is the constant they are compared against.

#include "counterwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Qt counter"));

    CounterWindow window;
    window.show();

    return QApplication::exec();
}
