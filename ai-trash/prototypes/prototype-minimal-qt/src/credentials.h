// The platform credential store, asynchronously.
//
// prototype-minimal asks the browser for a saved password on load
// (`navigator.credentials.get({ password: true, mediation: 'optional' })`) and
// silently re-derives the key from it, and hands the password back to the
// browser after a successful sign-in (`navigator.credentials.store`). Nothing
// in Qt itself does that, which is why this app used to always start locked.
// QtKeychain does, on all three of these targets:
//
//   Linux    GNOME Keyring, or KWallet over D-Bus
//   Android  the Android keystore — an RSA key in hardware wrapping an
//            AES-GCM key, the ciphertext in SharedPreferences
//   PWA      a transient HTML bridge form driving the browser's own password
//            manager and navigator.credentials — the same store
//            prototype-minimal reaches, reached the same way
//
// The secret stored is the password, not the derived key: the key is 310 000
// PBKDF2 iterations away and the vault blob is the only thing that can prove a
// password is right, so the app re-derives exactly as the web page does.
//
// Every call is a callback, because QtKeychain's jobs are asynchronous
// everywhere and on the PWA they are asynchronous for a hard reason — the
// bridge form waits for a person. read()'s `then` always runs exactly once,
// with an empty password when there was nothing to hand back; the caller needs
// the miss as much as the hit, because a miss is what opens the form.

#pragma once

#include <QString>

#include <functional>

namespace Cred {

// Whether a read can happen without the user seeing anything. True where the
// platform keychain answers on its own; false in the browser, where every read
// is a modal form and asking on startup would be worse than the Unlock button
// it replaces. Callers use this to decide whether to try at all, never to
// decide whether the store exists.
bool silent();

// Whether there is a backend at all. On Linux this is false with no session
// D-Bus and no keyring daemon — a headless container, for instance — and every
// job then fails with NoBackendAvailable.
bool available();

// key is the username, the same identity prototype-minimal gives its
// PasswordCredential, so a saved password shows up in the browser's and the
// desktop's password manager under a name a person recognises.
//
// `then` is called with an empty string for every kind of nothing there is:
// no entry, no backend, a locked keyring, a user who cancelled.
void read(const QString &key, std::function<void(const QString &password)> then);
void write(const QString &key, const QString &password,
           std::function<void(bool ok)> then = {});
void forget(const QString &key, std::function<void(bool ok)> then = {});

} // namespace Cred
