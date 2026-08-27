# prototype-minimal-qt

`prototype-minimal`, again — same vault, same sign-in rules, same roster parse
and rewrite, same 🔍🐛 fold, same two game cards — as a Qt application built for
three targets from one source tree.

| target | output | |
|---|---|---|
| PWA | `dist/pwa/` | 14 MB, installable, works offline |
| Linux | `dist/minimal-qt` | 185 kB native x86_64 |
| Android | `dist/minimal-qt-debug.apk` | 15 MB, `org.pgo.minimalqt`, arm64-v8a |

`dist/` is committed, so a fresh checkout can serve the PWA and install the APK
without a toolchain.

This is the fourth telling of the same app — JavaScript in `prototype-minimal`,
C compiled to wasm in `prototype-webassembly`, Kotlin in
`prototype-minimal-apk`, Qt here. The interesting number is how little new code
the fourth telling needed: **1,420 lines of C++ across `src/`**, none of which
knows what a roster is, what a vault entry looks like, or how AES works, plus a
280-line test suite.

## Where the app actually lives

Nothing was ported. `../prototype-webassembly/src/*.c` — 2,200 lines holding
the vault rules, the plaintext format, the `[group1]` keypair, PBKDF2/AES-GCM/
P-256 and the roster grammar — is compiled straight into this binary, and the
C++ calls it directly:

| | |
|---|---|
| **C, unchanged** | every rule and every cryptographic primitive |
| **C++/Qt** | widgets, storage, clipboard, entropy |

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

Built and checked on 2026-08-27:

- **PWA** — driven in headless Chromium: created an account (the vault and
  session land in `localStorage` in prototype-minimal's exact blob format,
  `v2 / PBKDF2-SHA256 / 310000`), reloaded and stayed signed in, opened the
  🔍🐛 fold and read `Readable: Cedric` and the `[group1]` P-256 key out of the
  decrypted panel, pasted a roster (2 date blocks, both cards filled) and
  toggled *Going* — 3 players, the rewritten roster 258 → 275 characters.
- **Linux** — `ctest` drives the real widgets offscreen: 9 test functions
  covering create, refuse, decline, two users on one password, restart,
  unlock, vault merge, roster toggle and delete-all.
- **Android** — assembles and is debug-signed (v2), `minSdk 28 / target 34`.
  Run on Cedric's phone, where it turned up the soft-keyboard bug below;
  otherwise unverified on hardware by anyone here.

## What Qt cannot do that a browser can

Three deviations, all of them forced, none of them in the rules:

- **No credential store.** `prototype-minimal` asks the browser for a saved
  password on load and silently re-derives the key. There is no
  `navigator.credentials` in Qt, so this app always starts locked and the
  **Unlock** button is the way back in — which is exactly what
  prototype-minimal does on Firefox and Safari.
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
until the composition was committed, which is what a focus change does. The
password field never had the problem, because `QLineEdit` adds
`ImhNoAutoUppercase | ImhNoPredictiveText | ImhSensitiveData` itself for any
echo mode that is not `Normal`. The same hints are now set explicitly on the
username field, the vault box and the paste box — which is also what
prototype-minimal asks the browser for on its username input
(`autocapitalize="none" autocorrect="off" spellcheck="false"`), and what
stopped typed text from being painted on top of the placeholder in the paste
box.

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

The key is never stored anywhere. A restart restores the session but not the
plaintext, which is why the decrypted panel starts locked.

## Building

Needs Qt 6 for three targets, Emscripten and the Android SDK/NDK — all in the
**`claude-code-agent-qt`** image. Either point the workspace at that image in
`workspaces.conf` and run `./build.sh`, or from an ordinary session:

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
| `deleteAllEmptiesTheVault` | both users gone, signed out, buttons greyed |

The rules underneath have their own suite in `prototype-webassembly`; these
check that this shell reaches them the way the web page does.

## Files

- `src/mainwindow.{h,cpp}` — the whole UI: account row, the folded panels, two
  cards, paste and output, the sign-in view, and the same five pieces of state
  the JavaScript shell keeps.
- `src/core.{h,cpp}` — the C core as C++ sees it. `wasm_reset()` before every
  call, results copied into `QString` before the next one, JSON in and out.
- `src/storage.{h,cpp}` — localStorage in the browser, QSettings elsewhere.
- `src/clipboard.{h,cpp}` — asynchronous clipboard, real Clipboard API in the
  browser.
- `src/main.cpp`, `CMakeLists.txt`, `build.sh`, `tests/`, `web/`, `android/`.

## Notes and limitations

- The PWA is 14 MB because Qt Widgets on the web is 14 MB; the app's own code
  is a rounding error in it. `prototype-webassembly` is the same app in 47 kB.
- Not styled. The look is stock Qt Fusion — same sections and same controls as
  prototype-minimal, none of its dark CSS.
- Sign-in blocks the UI thread for one PBKDF2 derivation per stored blob
  (310 000 iterations each), the same as everywhere else this app exists. The
  form disables itself and reads *Working…* while it runs.
- `dist/` is committed at Cedric's request, binaries included. Rebuilding
  changes ~30 MB of it, so `./build.sh` before a commit is a deliberate act.
- The APK is arm64-v8a and debug-signed: `adb install` yes, store no.
