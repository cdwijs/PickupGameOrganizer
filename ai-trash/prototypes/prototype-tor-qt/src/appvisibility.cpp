#include "appvisibility.h"

#ifdef Q_OS_WASM
#include <emscripten/html5.h>
#else
#include <QGuiApplication>
#endif

AppVisibility::AppVisibility(QObject *parent) : QObject(parent)
{
#ifdef Q_OS_WASM
    // Runs on the browser's main thread, which is also Qt's.
    emscripten_set_visibilitychange_callback(this, false, [](int, const EmscriptenVisibilityChangeEvent *event,
                                                             void *self) -> EM_BOOL {
        static_cast<AppVisibility *>(self)->setHidden(event->hidden);
        return false;
    });
#else
    // Inactive only means the window lost focus; ignore it.
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationSuspended || state == Qt::ApplicationHidden)
            setHidden(true);
        else if (state == Qt::ApplicationActive)
            setHidden(false);
    });
#endif
}

AppVisibility::~AppVisibility()
{
#ifdef Q_OS_WASM
    emscripten_set_visibilitychange_callback(nullptr, false, nullptr);
#endif
}

void AppVisibility::setHidden(bool isHidden)
{
    if (isHidden == m_hidden)
        return;
    m_hidden = isHidden;
    if (isHidden) {
        m_hiddenAtWall = QDateTime::currentMSecsSinceEpoch();
        m_hiddenAt.start();
        emit hidden();
    } else {
        emit shown(QDateTime::currentMSecsSinceEpoch() - m_hiddenAtWall, m_hiddenAt.elapsed());
    }
}
