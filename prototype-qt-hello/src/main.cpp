#include "core.h"

#include <QApplication>
#include <QLabel>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QLabel label(greeting());
    label.resize(320, 120);
    label.setAlignment(Qt::AlignCenter);
    label.show();
    return app.exec();
}
