#include "mainwindow.h"

#include <QApplication>

#include <csignal>

int main(int argc, char *argv[])
{
    // Embedded tor runs with __DisableSignalHandlers, so it no longer ignores
    // SIGPIPE itself; a write to a peer that hung up must not kill the app.
    std::signal(SIGPIPE, SIG_IGN);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("prototype-tor-qt"));

    MainWindow window;
#ifdef Q_OS_WASM
    window.showFullScreen(); // fills the page's container element
#else
    window.show();
#endif
    return app.exec();
}
