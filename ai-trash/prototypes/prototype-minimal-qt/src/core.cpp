#include "core.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QRandomGenerator>

namespace {

// The C reads and writes NUL-terminated UTF-8, which is what toUtf8() gives.
// The QByteArray has to outlive the call, so every helper keeps its own.
QString fromC(const char *p)
{
    return p ? QString::fromUtf8(p) : QString();
}

QJsonObject asObject(const char *p)
{
    return QJsonDocument::fromJson(p ? QByteArray(p) : QByteArray("{}")).object();
}

// Errors come back as {"error": "..."} and hits as {"id","username","text"},
// with "vault" added when the call produced a new one.
Account asAccount(const QJsonObject &o)
{
    Account a;
    a.error = o.value(QStringLiteral("error")).toString();
    a.id = o.value(QStringLiteral("id")).toString();
    a.username = o.value(QStringLiteral("username")).toString();
    a.text = o.value(QStringLiteral("text")).toString();
    a.vault = o.value(QStringLiteral("vault")).toString();
    return a;
}

QByteArray entropy(int n)
{
    QByteArray b(n, Qt::Uninitialized);
    QRandomGenerator::system()->generate(b.begin(), b.end());
    return b;
}

const proto_u8 *raw(const QByteArray &b)
{
    return reinterpret_cast<const proto_u8 *>(b.constData());
}

} // namespace

namespace Core {

Account create(const QString &vault, const QString &username, const QString &password)
{
    const QByteArray v = vault.toUtf8(), u = username.toUtf8(), p = password.toUtf8();
    // The module needs its own randomness: an id, a salt, a nonce and the
    // candidate scalars for the keypair.
    const QByteArray rnd = entropy(rnd_needed());
    wasm_reset();
    return asAccount(asObject(account_create(v.constData(), u.constData(), p.constData(), raw(rnd))));
}

Account signIn(const QString &vault, const QString &username, const QString &password)
{
    const QByteArray v = vault.toUtf8(), u = username.toUtf8(), p = password.toUtf8();
    wasm_reset();
    return asAccount(asObject(account_signin(v.constData(), u.constData(), p.constData())));
}

Account unlock(const QString &vault, const QString &id, const QString &password)
{
    const QByteArray v = vault.toUtf8(), i = id.toUtf8(), p = password.toUtf8();
    wasm_reset();
    return asAccount(asObject(account_unlock(v.constData(), i.constData(), p.constData())));
}

int count(const QString &vault)
{
    const QByteArray v = vault.toUtf8();
    wasm_reset();
    return vault_count(v.constData());
}

bool has(const QString &vault, const QString &id)
{
    const QByteArray v = vault.toUtf8(), i = id.toUtf8();
    wasm_reset();
    return vault_has(v.constData(), i.constData()) == 1;
}

QString text(const QString &vault)
{
    const QByteArray v = vault.toUtf8();
    wasm_reset();
    return fromC(vault_text(v.constData()));
}

QString remove(const QString &vault, const QString &id)
{
    const QByteArray v = vault.toUtf8(), i = id.toUtf8();
    wasm_reset();
    return fromC(vault_delete(v.constData(), i.constData()));
}

Ingest ingest(const QString &vault, const QString &pasted)
{
    const QByteArray v = vault.toUtf8(), t = pasted.toUtf8();
    // Merging can mint ids for records pasted without one, so it wants entropy
    // too — the shell hands over a fixed 512 bytes, same as the JS one.
    const QByteArray rnd = entropy(512);
    wasm_reset();
    const QJsonObject o = asObject(vault_ingest(v.constData(), t.constData(), raw(rnd), 512));

    Ingest r;
    r.error = o.value(QStringLiteral("error")).toString();
    r.vault = o.value(QStringLiteral("vault")).toString();
    r.added = o.value(QStringLiteral("added")).toInt();
    r.replaced = o.value(QStringLiteral("replaced")).toInt();
    return r;
}

QList<Block> parse(const QString &rawText, const QString &username, QString *out)
{
    const QByteArray t = rawText.toUtf8(), u = username.toUtf8();
    wasm_reset();
    const int n = export_roster_parse(t.constData(), u.constData());

    QList<Block> blocks;
    blocks.reserve(n);
    for (int i = 0; i < n; ++i) {
        const QJsonObject o = asObject(export_roster_info(i));
        Block b;
        b.date = o.value(QStringLiteral("date")).toString();
        b.weekday = o.value(QStringLiteral("weekday")).toString();
        b.time = o.value(QStringLiteral("time")).toString();
        b.count = o.value(QStringLiteral("count")).toInt();
        b.going = o.value(QStringLiteral("going")).toBool();
        blocks.append(b);
    }
    // The rewritten text comes out of the same arena pass.
    if (out)
        *out = fromC(export_roster_out());
    return blocks;
}

QString toggle(QString *rawText, const QString &username, int idx)
{
    {
        const QByteArray t = rawText->toUtf8(), u = username.toUtf8();
        wasm_reset();
        export_roster_parse(t.constData(), u.constData());
        export_roster_flip(idx);
        *rawText = fromC(export_roster_out());
    }
    QString out;
    parse(*rawText, username, &out);
    return out;
}

QString displayName(const QString &username)
{
    const QByteArray u = username.toUtf8();
    wasm_reset();
    return fromC(export_display_name(u.constData()));
}

} // namespace Core
