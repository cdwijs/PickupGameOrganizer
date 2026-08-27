// The C core, as C++ sees it.
//
// Every rule this app has — the vault, the plaintext format, the [group1]
// keypair, PBKDF2/AES-GCM/P-256, the roster grammar — lives in
// ../../prototype-webassembly/src/*.c and is compiled into this binary
// unchanged. The declarations below are that module's exported surface; the
// wrapper under them is only string conversion and JSON.
//
// The C keeps one arena that is reset before every operation, so a result is
// only valid until the next call. Core copies each one into a QString on the
// way out, which is what the JavaScript shell does too.

#pragma once

#include <QJsonObject>
#include <QString>

extern "C" {
using proto_u8 = unsigned char;

void wasm_reset(void);

int rnd_needed(void);
const char *account_create(const char *vault_json, const char *username,
                           const char *password, const proto_u8 *rnd);
const char *account_signin(const char *vault_json, const char *username,
                           const char *password);
const char *account_unlock(const char *vault_json, const char *id,
                           const char *password);

int vault_count(const char *vault_json);
int vault_has(const char *vault_json, const char *id);
const char *vault_text(const char *vault_json);
const char *vault_delete(const char *vault_json, const char *id);
const char *vault_ingest(const char *vault_json, const char *text,
                         const proto_u8 *rnd, int rnd_len);

int export_roster_parse(const char *text, const char *username);
const char *export_roster_info(int idx);
void export_roster_flip(int idx);
const char *export_roster_out(void);
const char *export_display_name(const char *username);
}

// One date block as the module reports it.
struct Block
{
    QString date;
    QString weekday;
    QString time;
    int count = 0;
    bool going = false;
};

// What a sign-in, an unlock or a create produced: either an error code or an
// opened blob.
struct Account
{
    QString error;      // "nomatch", "badpassword", "gone", or a message
    QString id;
    QString username;
    QString text;       // the decrypted plaintext
    QString vault;      // the vault to store, when the call changed it
    bool ok() const { return error.isEmpty(); }
};

struct Ingest
{
    QString error;
    QString vault;
    int added = 0;
    int replaced = 0;
};

namespace Core {

// Accounts and the vault.
Account create(const QString &vault, const QString &username, const QString &password);
Account signIn(const QString &vault, const QString &username, const QString &password);
Account unlock(const QString &vault, const QString &id, const QString &password);

int count(const QString &vault);
bool has(const QString &vault, const QString &id);
QString text(const QString &vault);                    // pretty-printed vault
QString remove(const QString &vault, const QString &id);
Ingest ingest(const QString &vault, const QString &pasted);

// The roster. parse() returns the blocks and the rewritten text together,
// because normalising the user's own slots can change the text even when
// nothing was toggled.
QList<Block> parse(const QString &raw, const QString &username, QString *out);
QString toggle(QString *raw, const QString &username, int idx);

QString displayName(const QString &username);

} // namespace Core
