#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("prototype-webrtc-stun"));

    MainWindow window;
#ifdef Q_OS_WASM
    window.showFullScreen(); // fills the page's container element
#else
    window.show();
#endif
    return app.exec();
}
