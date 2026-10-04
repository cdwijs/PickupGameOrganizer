#include "credentials.h"

#include <qtkeychain/keychain.h>

#include <QCoreApplication>

namespace {

// One namespace for every key this app stores, the way QtKeychain's service
// string is meant to be used. It matches the Storage:: prefix so a keyring
// entry is recognisable next to the QSettings file it belongs to.
const QString &service()
{
    static const QString s = QStringLiteral("prototype-minimal-qt");
    return s;
}

// Jobs autoDelete, so nothing here owns them; the application is the parent
// only so a job cannot outlive the event loop it is queued in.
QObject *owner()
{
    return QCoreApplication::instance();
}

} // namespace

namespace Cred {

bool silent()
{
#ifdef __EMSCRIPTEN__
    // QtKeychain's WebAssembly backend is a modal bridge form: it has to be,
    // because navigator.credentials.store() needs a user gesture and browsers
    // only offer to save a password when they see a form submitted. A read is
    // therefore never silent here, and asking on every page load would be a
    // dialog in the user's way where today there is a button they can ignore.
    return false;
#else
    return true;
#endif
}

bool available()
{
    return QKeychain::isAvailable();
}

void read(const QString &key, std::function<void(const QString &password)> then)
{
    auto *job = new QKeychain::ReadPasswordJob(service(), owner());
    job->setKey(key);
    // Never a plaintext file: a missing backend is reported as a failure and
    // the app falls back to asking, rather than to storing the password in the
    // clear next to the vault it opens.
    job->setInsecureFallback(false);
    QObject::connect(job, &QKeychain::Job::finished, owner(),
                     [then = std::move(then)](QKeychain::Job *finished) {
                         auto *read = static_cast<QKeychain::ReadPasswordJob *>(finished);
                         if (then)
                             then(read->error() ? QString() : read->textData());
                     });
    job->start();
}

void write(const QString &key, const QString &password, std::function<void(bool ok)> then)
{
    auto *job = new QKeychain::WritePasswordJob(service(), owner());
    job->setKey(key);
    job->setTextData(password);
    job->setInsecureFallback(false);
    QObject::connect(job, &QKeychain::Job::finished, owner(),
                     [then = std::move(then)](QKeychain::Job *finished) {
                         if (then)
                             then(finished->error() == QKeychain::NoError);
                     });
    job->start();
}

void forget(const QString &key, std::function<void(bool ok)> then)
{
    auto *job = new QKeychain::DeletePasswordJob(service(), owner());
    job->setKey(key);
    job->setInsecureFallback(false);
    QObject::connect(job, &QKeychain::Job::finished, owner(),
                     [then = std::move(then)](QKeychain::Job *finished) {
                         // EntryNotFound is the state the caller wanted: there
                         // is nothing stored under this key any more.
                         const QKeychain::Error error = finished->error();
                         if (then)
                             then(error == QKeychain::NoError
                                  || error == QKeychain::EntryNotFound);
                     });
    job->start();
}

} // namespace Cred
