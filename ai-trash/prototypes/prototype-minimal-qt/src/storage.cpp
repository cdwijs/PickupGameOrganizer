#include "storage.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// Qt's QSettings has no localStorage backend, and the browser is the one place
// where the storage is worth being able to read from the console, so these
// three go straight to it. Values are UTF-8 and can be long (the vault is one
// JSON array), which is what UTF8ToString / stringToNewUTF8 handle.
EM_JS(char *, ls_get, (const char *key), {
    try {
        const v = localStorage.getItem(UTF8ToString(key));
        return v === null ? 0 : stringToNewUTF8(v);
    } catch (e) {
        return 0;
    }
});

EM_JS(void, ls_set, (const char *key, const char *value), {
    try {
        localStorage.setItem(UTF8ToString(key), UTF8ToString(value));
    } catch (e) {
        // A full or blocked store is not fatal here: the app reports what it
        // could not keep the same way prototype-minimal does.
    }
});

EM_JS(void, ls_remove, (const char *key), {
    try {
        localStorage.removeItem(UTF8ToString(key));
    } catch (e) {
    }
});

namespace Storage {

QString get(const QString &key)
{
    char *v = ls_get(key.toUtf8().constData());
    if (!v)
        return QString();
    const QString out = QString::fromUtf8(v);
    free(v);
    return out;
}

void set(const QString &key, const QString &value)
{
    ls_set(key.toUtf8().constData(), value.toUtf8().constData());
}

void remove(const QString &key)
{
    ls_remove(key.toUtf8().constData());
}

} // namespace Storage

#else
#include <QSettings>

namespace {
QSettings &settings()
{
    // Same file for every call; QSettings syncs on destruction and on sync().
    static QSettings s(QStringLiteral("pgo"), QStringLiteral("prototype-minimal-qt"));
    return s;
}
} // namespace

namespace Storage {

QString get(const QString &key)
{
    return settings().value(key).toString();
}

void set(const QString &key, const QString &value)
{
    settings().setValue(key, value);
    settings().sync();
}

void remove(const QString &key)
{
    settings().remove(key);
    settings().sync();
}

} // namespace Storage
#endif
