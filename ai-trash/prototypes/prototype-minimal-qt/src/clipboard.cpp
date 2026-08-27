#include "clipboard.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

namespace {
// One read at a time is enough: the button that starts it is disabled by the
// modal that a failure raises, and a second click simply replaces the first.
std::function<void(const QString &, bool)> g_pending;
} // namespace

// Called back from the JS below once the promise settles.
extern "C" EMSCRIPTEN_KEEPALIVE void proto_clipboard_result(const char *text, int ok)
{
    auto callback = std::move(g_pending);
    g_pending = nullptr;
    if (callback)
        callback(text ? QString::fromUtf8(text) : QString(), ok != 0);
}

EM_JS(void, clip_read, (), {
    const deliver = (text, ok) => {
        const p = stringToNewUTF8(text || "");
        _proto_clipboard_result(p, ok ? 1 : 0);
        if (typeof _free === "function")
            _free(p);
    };
    try {
        navigator.clipboard.readText().then(t => deliver(t, true), () => deliver("", false));
    } catch (e) {
        deliver("", false);
    }
});

EM_JS(void, clip_write, (const char *text), {
    try {
        navigator.clipboard.writeText(UTF8ToString(text));
    } catch (e) {
        // Denied or unsupported: the page has no way to force it, and neither
        // does prototype-minimal.
    }
});

namespace Clip {

void write(const QString &text)
{
    clip_write(text.toUtf8().constData());
}

void read(std::function<void(const QString &, bool)> then)
{
    g_pending = std::move(then);
    clip_read();
}

} // namespace Clip

#else
#include <QClipboard>
#include <QGuiApplication>

namespace Clip {

void write(const QString &text)
{
    QGuiApplication::clipboard()->setText(text);
}

void read(std::function<void(const QString &, bool)> then)
{
    // Synchronous here, but answered through the same callback so the caller
    // is written once.
    then(QGuiApplication::clipboard()->text(), true);
}

} // namespace Clip
#endif
