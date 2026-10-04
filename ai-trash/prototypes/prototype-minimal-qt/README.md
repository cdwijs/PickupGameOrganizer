# prototype-minimal-qt

`prototype-minimal`, again — same vault, same sign-in rules, same roster parse
and rewrite, same 🔍🐛 fold, same two game cards — as a Qt application built for
three targets from one source tree.

| target | output | |
|---|---|---|
| PWA | `dist/pwa/` | 14 MB, installable, works offline |
| Linux | `dist/minimal-qt` | 450 kB native x86_64 |
| Android | `dist/minimal-qt-debug.apk` | 15 MB, `org.pgo.minimalqt`, arm64-v8a |

`dist/` is committed, so a fresh checkout can serve the PWA and install the APK
without a toolchain.

This is the fourth telling of the same app — JavaScript in `prototype-minimal`,
C compiled to wasm in `prototype-webassembly`, Kotlin in
`prototype-minimal-apk`, Qt here. The interesting number is how little new code
the fourth telling needed: **1,570 lines of C++ across `src/`**, none of which
knows what a roster is, what a vault entry looks like, or how AES works, plus a
500-line test suite. The credential store is 150 lines of that, and none of the
platform work is in it — see below.

## Where the app actually lives

Nothing was ported. `../prototype-webassembly/src/*.c` — 2,200 lines holding
the vault rules, the plaintext format, the `[group1]` keypair, PBKDF2/AES-GCM/
P-256 and the roster grammar — is compiled straight into this binary, and the
C++ calls it directly:

| | |
|---|---|
| **C, unchanged** | every rule and every cryptographic primitive |
| **C++/Qt** | widgets, storage, clipboard, entropy, the credential store |

That C is already differential-tested against prototype-minimal's own
JavaScript (99 comparisons of parse/toggle/rewrite, plus both directions of
blob interop), so "behaves exactly like prototype-minimal" is a property of
what is compiled in rather than of a fresh translation. A vault written here
opens in prototype-minimal and the other way round.

One line changed in the sibling: `base.c` defines `memcpy`/`memset`/`memmove`
for its freestanding wasm build, and every target here has a libc that already
has them, so they now sit behind `#ifndef PROTO_HOSTED`. `prototype-webassembly`
builds byte-identically and all five of its suites still pass.

## Build status

Built and checked on 2026-08-27, and again on 2026-09-09 for the credential
store:

- **PWA** — driven in headless Chromium: created an account (the vault and
  session land in `localStorage` in prototype-minimal's exact blob format,
  `v2 / PBKDF2-SHA256 / 310000`), reloaded and stayed signed in, opened the
  🔍🐛 fold and read `Readable: Cedric` and the `[group1]` P-256 key out of the
  decrypted panel, pasted a roster (2 date blocks, both cards filled) and
  toggled *Going* — 3 players, the rewritten roster 258 → 275 characters.
  Driven again for the credential store: creating the account raised
  QtKeychain's *Save* form with `Cedric` and the password already in it,
  reloading showed **no dialog** and the *Unlock* button, and pressing *Unlock*
  raised the *Sign In* form — with the browser's own *Use a saved password…*
  button on it — whose password refilled the decrypted panel (560 chars) and
  left no second modal behind.
- **Linux** — `ctest` drives the real widgets offscreen: 15 test functions
  covering create, refuse, decline, two users on one password, restart,
  unlock, vault merge, roster toggle, delete-all, and seven for the keyring.
- **Android** — assembles and is debug-signed (v2), `minSdk 28 / target 34`.
  `keychain_android.cpp` and `androidkeystore.cpp` are in the packaged
  `libminimal-qt_arm64-v8a.so`. Run on Cedric's phone before the keyring
  landed, where it turned up the soft-keyboard bug below; the keystore path
  itself is unverified on hardware by anyone here.

## What Qt cannot do that a browser can

Two deviations, both forced, neither in the rules:

- **Clipboard reads can be refused.** `Clip::read()` is asynchronous on every
  target because in the browser it has to be: `QClipboard::text()` returns only
  what a paste event already gave Qt, so an app-initiated read comes back
  empty. In the browser it calls `navigator.clipboard.readText()`, the same
  call prototype-minimal makes, and reports a refusal the same way. (Headless
  Chromium denies `clipboard-read` outright, which is how that path got
  tested.) Ctrl+V into the box always works.
- **No emoji in the wasm font.** The WebAssembly build carries one bundled
  font with no emoji coverage, so 🔍🐛 comes out as two empty boxes. The button
  asks `QFontMetrics::inFontUcs4()` first and falls back to the label `debug`;
  the 🗓️ and ⚽ inside pasted roster text are still tofu there.

## The credential store

`prototype-minimal` asks the browser for a saved password on load
(`navigator.credentials.get({ password: true, mediation: 'optional' })`) and
silently re-derives the key from it, and hands the password back after a
successful sign-in (`navigator.credentials.store`). This app used to have no
equivalent and always started locked — that was the third forced deviation
above, and it is gone. [QtKeychain][qtk] is a submodule under `third_party/`,
and it has a backend for every one of these three targets:

| target | where the password goes |
|---|---|
| Linux | GNOME Keyring, or KWallet over D-Bus |
| Android | the Android keystore — a hardware-backed RSA key wrapping an AES-GCM key, the ciphertext in `SharedPreferences` |
| PWA | the browser's own password manager, through `navigator.credentials` and a transient HTML form — the same store prototype-minimal reaches, reached the same way |

[qtk]: https://github.com/frankosterfeld/qtkeychain

**What is stored is the password, not the key.** The key is 310 000 PBKDF2
iterations away and the blob is the only thing that can prove a password right,
so the app re-derives on every start exactly as the web page does. The entry is
keyed by the username the *blob* holds rather than the one that was typed, so
signing in as `CEDRIC` does not leave a second entry behind.

**It is consulted in two places, and not a third.**

- **At startup, on a restored session.** The session names the user, the keyring
  hands back the password, one derivation fills the decrypted panel. This is
  precisely where prototype-minimal calls `tryPrefill()`.
- **When Unlock is pressed.** The keyring is asked first; only if it has
  nothing, or hands back a password the blob refuses, does the form open.
- **Never from the sign-in view.** A keyring is looked up by name, and there is
  nothing to enumerate — the vault stores every username encrypted inside its
  own blob. Without a session there is no name to look up, so a first sign-in on
  a device is always typed.

**The PWA reads only on Unlock.** QtKeychain's WebAssembly backend has to be a
modal form: `navigator.credentials.store()` needs a user gesture, and browsers
only offer to save a password when they see a form submitted. Reading on every
page load would therefore put a dialog in the way where today there is a button
that can be ignored, so `Cred::silent()` is false there and the startup read is
skipped. Unlock is the user gesture the browser wants, and the button the store
hangs off.

That also decides one thing that looks like a detail and is not: a password
that came *out* of the store is never written back. It is already there, and on
the PWA the write is a second modal — an unlock would close the *Sign In* form
only to open a *Save* one. `MainWindow::PasswordSource` is what carries that
distinction from the sign-in attempt down to `finishSignIn()`.

**Sign out keeps the entry; deleting a user clears it.** Signing out is durable
without deleting anything, because the startup read needs a session to name the
user and signing out removes it. Deleting a user removes the password with the
blob, since a secret with nothing left to open is a stray. Two things this
cannot do:

- **Delete-all clears only the signed-in user's entry**, for the same reason
  the sign-in view cannot be prefilled: the other usernames are inside blobs
  that are about to be erased, so there is no way to name them. A second user's
  saved password is left behind with nothing to open.
- **The PWA cannot delete at all** — `DeletePasswordJob` reports
  `NotImplemented`, because a page cannot reach into the browser's password
  manager. Clearing it there is a thing the user does in browser settings.

**Nothing is ever written in the clear.** `setInsecureFallback(false)` on every
job, so a machine with no keyring — a build container, a headless session —
reports a failure and the app falls back to asking, rather than dropping the
password into a settings file next to the vault it opens.

## Typing on Android

Two things had to be fixed before the sign-in form could be filled in on a
phone, and only the second one was the real bug.

**The keyboard has to be asked for.** Android raises it only on request. A
programmatic `setFocus()` is not a tap, and a tap on a field that already holds
focus changes no focus, so neither route reached the platform.
`focusField()` now calls `QInputMethod::show()` with every programmatic focus,
and an event filter on the four editable widgets asks again on tap and
focus-in, which is also how the keyboard comes back after being dismissed. The
activity is `android:windowSoftInputMode="adjustResize"` and the sign-in view
has its own `QScrollArea`, so the keyboard shortens the form rather than
covering it.

**The username field looked dead while typing.** Characters went in and nothing
appeared; moving focus to the password field made everything typed show up at
once in the username box. Android's keyboard *composes* text before committing
it, and the composing string was not being drawn — so the field stayed empty
until the composition was committed, which is what a focus change does.

The first attempt at this set `ImhNoAutoUppercase | ImhNoPredictiveText` and
**changed nothing on the phone**, for a reason worth writing down: Qt turns
`ImhNoPredictiveText` into Android's `TYPE_TEXT_FLAG_NO_SUGGESTIONS` only when
the environment variable
`QT_ANDROID_ENABLE_WORKAROUND_TO_DISABLE_PREDICTIVE_TEXT` is set
(`QtEditText.isDisablePredictiveTextWorkaround`, in `Qt6Android.jar`), and
nothing sets it. The hint is inert by default.

`Qt::ImhSensitiveData` needs no opt-in: Qt maps it to
`TYPE_TEXT_VARIATION_VISIBLE_PASSWORD`, an input type keyboards do not compose
in, so each character is committed as it is typed. That is also why the
password field was never affected — `QLineEdit` adds `ImhHiddenText` for any
echo mode that is not `Normal`, which maps to the password input type. It is
now set on the username field, the vault box and the paste box, alongside the
hints prototype-minimal asks the browser for on the same input
(`autocapitalize="none" autocorrect="off" spellcheck="false"`).

None of this does anything on desktop.

## Dialogs are asynchronous, and have to be

`QMessageBox::warning()` and `::question()` block on a nested event loop. Qt for
WebAssembly has no nested event loop, so in the browser the first version of
this app opened an empty grey dialog frame with a title bar and nothing in it,
and the sign-in never came back. Every dialog now goes through two hooks —

```cpp
virtual void notify(title, text, std::function<void()> then = {});
virtual void confirm(title, text, std::function<void(bool)> then);
```

— which `open()` the box and resume in the continuation. That is why
`attemptSignIn()` is written in continuation style: the failure notice, the
offer to create, and the create itself are three steps with a user in between
each. The tests subclass those two hooks and answer immediately, so every flow
still runs inside the click that started it.

## Storage

Three keys, the same three prototype-minimal keeps:

| key | contents |
|---|---|
| `prototype-minimal-qt:vault:v1` | the vault — one entry per user, `{id, salt, blob}` |
| `prototype-minimal-qt:session:v1` | `{"id":…,"username":…}`, so a restart stays signed in |
| `prototype-minimal-qt:debug:v1` | `"1"` / `"0"` — is the 🔍🐛 fold open |

In the browser they are real `localStorage` keys (`EM_JS`, because QSettings has
no localStorage backend), under this prototype's own namespace so a Qt PWA and
prototype-minimal can be open in one browser without fighting over one vault.
On Linux and Android they are `QSettings` — an INI file under
`~/.config/pgo/`, `SharedPreferences`-backed on Android.

The key is never stored anywhere, and neither is the password: a restart
restores the session but not the plaintext. What refills it is a fresh
derivation from a password the *platform's* keyring hands back — never one of
these three keys. See [The credential store](#the-credential-store).

## Building

Needs Qt 6 for three targets, Emscripten and the Android SDK/NDK — all in the
**`claude-code-agent-qt`** image — and the QtKeychain submodule:

```sh
git submodule update --init \
    ai-trash/prototypes/prototype-minimal-qt/third_party/qtkeychain
```

`build.sh` fetches it itself if it is missing, and CMake refuses to configure
without it. It is pinned to `0deb2c0` rather than to a tag: the WebAssembly
backend landed after v0.14.0 and is in no release yet, and it is the reason all
three targets can have a credential store from one dependency. It is built
static, with its own translations and tests off, and with `LIBSECRET_SUPPORT=OFF`
— libsecret is not in the Qt image, and without it the Linux backend still
reaches GNOME Keyring and KWallet over D-Bus, which is what a desktop session
actually runs.

Either point the workspace at that image in `workspaces.conf` and run
`./build.sh`, or from an ordinary session:

```sh
./build.sh --docker              # linux + tests + pwa + android → dist/
./build.sh --docker test         # just the widget tests
./build.sh --docker pwa          # just the PWA
```

Deliverables land in `dist/`; the build trees and Gradle's 400 MB cache stay in
`/shared/tmp/prototype-minimal-qt/`. See
[`../prototype-qt/README.md`](../prototype-qt/README.md) for what `--docker`
does, why the Emscripten version has to match Qt's exactly, and where the 14 MB
of wasm goes.

### Running it

```sh
cd dist/pwa && python3 -m http.server 8080      # then open http://localhost:8080
./dist/minimal-qt
adb install -r dist/minimal-qt-debug.apk
```

## Tests

```sh
./build.sh --docker test
```

`tests/tst_minimal.cpp` drives the real widgets under
`QT_QPA_PLATFORM=offscreen`, clicking the buttons a user clicks:

| | |
|---|---|
| `createsAUserAndSignsIn` | empty vault → *Incorrect username or password* → offer → one blob, plaintext in the panel |
| `wrongPasswordIsRefusedAndOffersToCreate` | declining the offer changes nothing |
| `secondUserIsAddedNotOverwritten` | two users on one password; the stored spelling wins over the typed case |
| `sessionSurvivesRestartButTheKeyDoesNot` | restart: signed in, plaintext gone, Unlock offered, fold state remembered |
| `unlockRefillsThePlaintext` | wrong password says *Incorrect password* and never offers to create; the right one refills |
| `pastedVaultIsMerged` | a vault pasted into an empty device adopts, twice is a no-op, and it still opens with its password |
| `rosterParsesAndTogglesGoing` | toggle adds `Cedric (app)` to the first block and removes it again |
| `deleteAllEmptiesTheVault` | both users gone, signed out, buttons greyed — and only the signed-in user's saved password went with them |
| `signingInSavesThePasswordInTheKeyring` | one entry, under the spelling the blob holds and not the one typed |
| `savedPasswordUnlocksOnRestart` | a restart reads the keyring once and refills the plaintext: no form, no notice, no Unlock button |
| `aStoreThatIsNotSilentIsLeftForTheUnlockButton` | the PWA's rule — nothing is asked of the store on a page load |
| `unlockUsesTheSavedPassword` | Unlock unlocks straight from the store, never shows the form, and does not write the password back |
| `aStalePasswordFallsBackToTheForm` | a password the blob refuses fails silently at startup, makes Unlock ask, and is replaced by the next success |
| `deletingAUserForgetsItsSavedPassword` | the blob and its password go together |
| `aRealReadAlwaysCallsBack` | the real `Cred::read()`, not the fake: a miss has to come back as a miss, because every fallback in the app hangs off it |

The rules underneath have their own suite in `prototype-webassembly`; these
check that this shell reaches them the way the web page does.

## Files

- `src/mainwindow.{h,cpp}` — the whole UI: account row, the folded panels, two
  cards, paste and output, the sign-in view, and the same five pieces of state
  the JavaScript shell keeps.
- `src/core.{h,cpp}` — the C core as C++ sees it. `wasm_reset()` before every
  call, results copied into `QString` before the next one, JSON in and out.
- `src/storage.{h,cpp}` — localStorage in the browser, QSettings elsewhere.
- `src/credentials.{h,cpp}` — the saved password. Four calls over QtKeychain;
  `silent()` is the whole of what this app knows about the platforms.
- `src/clipboard.{h,cpp}` — asynchronous clipboard, real Clipboard API in the
  browser.
- `src/main.cpp`, `CMakeLists.txt`, `build.sh`, `tests/`, `web/`, `android/`.
- `third_party/qtkeychain` — submodule, pinned to `0deb2c0`.

## Notes and limitations

- The PWA is 14 MB because Qt Widgets on the web is 14 MB; the app's own code
  is a rounding error in it. `prototype-webassembly` is the same app in 47 kB.
- Not styled. The look is stock Qt Fusion — same sections and same controls as
  prototype-minimal, none of its dark CSS.
- Sign-in blocks the UI thread for one PBKDF2 derivation per stored blob
  (310 000 iterations each), the same as everywhere else this app exists. The
  form disables itself and reads *Working…* while it runs.
- **The C core is compiled `-O2` even in a Debug build** (`CMakeLists.txt`).
  The APK has to be a Debug build to stay debuggable — Qt passes `--release`
  to `androiddeployqt` for every other configuration — and `-O0` through
  310 000 PBKDF2 iterations turned sign-in on a phone into a half-minute wait.
  Only the crypto is optimised; the C++ a debugger is pointed at is not.
- `dist/` is committed at Cedric's request, binaries included. Rebuilding
  changes ~30 MB of it, so `./build.sh` before a commit is a deliberate act.
- The APK is arm64-v8a and debug-signed: `adb install` yes, store no.
- The Linux binary went from 185 kB to 450 kB, which is QtKeychain linked
  static plus Qt6::DBus. The PWA and the APK did not change size in any way
  worth reporting: 14 MB and 15 MB are Qt.
- `QKeychain::isAvailable()` is called once per launch, and on a desktop
  session it is two blocking D-Bus round trips (KWallet 6, then KWallet 5) on
  the UI thread. In a container with no session bus it short-circuits. It has
  not been a visible pause anywhere it has run, but it is a synchronous call in
  a startup path and worth knowing about.
- The keystore path on Android is the one part of this that no one has run on
  hardware. Everything about it is in QtKeychain rather than here, but "it
  compiles into the APK" is all that has been shown.
