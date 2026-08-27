// prototype-minimal, as a Qt application.
//
// The rules are not here and not in any C++ file: they are in
// ../../prototype-webassembly/src/*.c, compiled into this binary unchanged.
// What Qt adds is the part a wasm module cannot do — widgets, storage, the
// clipboard and entropy.

#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("pgo"));
    QApplication::setApplicationName(QStringLiteral("prototype-minimal-qt"));

    MainWindow window;
    window.show();

    return QApplication::exec();
}
