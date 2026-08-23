# prototype-minimal-apk

`prototype-minimal` as a native Android app: Kotlin UI, the platform's crypto,
and the same vault format, so a vault moves between this and the web and
WebAssembly prototypes.

> ## ⚠ Not yet compiled
>
> **This project has never been built.** The container it was written in has no
> JDK, no Android SDK and no Gradle, so not one line here has been through a
> compiler — no `assembleDebug`, no lint, no run on a device. Expect the usual
> first-build errors.
>
> Everything else in this repo was verified before it was committed; this is
> the exception, and it is not one to paper over. See **Building** for what the
> image needs.

Named `prototype-minimal-apk` because `prototype-apk` is already the Android
port of `prototype-arti`.

## What it is

The same app, rebuilt on the platform's own APIs rather than the browser's:

| | prototype-minimal | here |
|---|---|---|
| storage | `localStorage` | `SharedPreferences` |
| crypto | WebCrypto | `javax.crypto` / `java.security` |
| clipboard | async Clipboard API | `ClipboardManager` |
| password autofill | Credential Management API | Android autofill |
| UI | HTML + CSS | Kotlin + XML layouts |

Feature for feature: create and sign in, wrong password refused without saying
which half was wrong, the vault box with copy/paste and its merge rules, delete
one and delete all, a session that survives a restart, the Unlock button, the
🔍🐛 debug fold, the roster parse and rewrite with `(app)` names, the two game
cards, and a `[group1]` ECDSA P-256 keypair for every new account.

## Building

The image needs a JDK and the Android SDK. Neither is in the container today —
add them to the Dockerfile rather than installing by hand, so they ship with
the image:

```dockerfile
RUN apt-get update && apt-get install -y --no-install-recommends \
        openjdk-17-jdk-headless unzip && rm -rf /var/lib/apt/lists/*

ENV ANDROID_HOME=/opt/android-sdk
RUN mkdir -p $ANDROID_HOME/cmdline-tools && cd /tmp \
 && curl -fsSLO https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip \
 && unzip -q commandlinetools-linux-*.zip -d $ANDROID_HOME/cmdline-tools \
 && mv $ANDROID_HOME/cmdline-tools/cmdline-tools $ANDROID_HOME/cmdline-tools/latest \
 && yes | $ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager --licenses \
 && $ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager \
      "platform-tools" "platforms;android-34" "build-tools;34.0.0"
ENV PATH=$PATH:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools
```

No NDK is needed — there is no native code here, unlike `prototype-apk`.

Then:

```sh
./gradlew assembleDebug
# app/build/outputs/apk/debug/app-debug.apk
```

`gradle-wrapper.jar` is **not** committed (the sibling project does the same),
so `./gradlew` needs the jar restoring first — `gradle wrapper` with a system
Gradle, or opening the project once in Android Studio.

## Interoperable vaults

Same PBKDF2-SHA256 at 310 000 iterations, same AES-256-GCM with the nonce
prefixed, same record shape, same plaintext with its `[group1]` section. Copy
the vault out of the browser prototype, paste it into the box here, and the
accounts open — and the other way round.

One detail worth spelling out, because it is the classic way this breaks:
**PBKDF2 is implemented over `Mac("HmacSHA256")` rather than through
`SecretKeyFactory`/`PBEKeySpec`.** `PBEKeySpec` takes a `CharArray`, and how
those characters become bytes has differed between providers — some use the low
byte of each character, some UTF-8. WebCrypto hashes the UTF-8 bytes, full
stop. Deriving over `password.toByteArray(UTF_8)` here removes the ambiguity
for twenty lines of code, and keeps non-ASCII passwords compatible.

The `SharedPreferences` keys are the app's own, so nothing collides.

## Differences from the web prototype

- **No silent re-unlock on launch.** The browser version can read a saved
  credential through the Credential Management API and re-derive the key with
  no interaction. Android's autofill framework does not work that way: it fills
  a form the user is looking at. So a restarted session shows *locked* and the
  Unlock button, with autofill offering the password once the field has focus.
- **The private key of `group1` is in `SharedPreferences`, inside the encrypted
  blob** — the same place the web version keeps it, not the Android Keystore.
  Using the Keystore would be the right move for a real app, and would also
  make the vault non-portable, which is the point of the format being shared.
- Dialogs are `AlertDialog` rather than `alert()`/`confirm()`, so they are
  asynchronous; the create-user flow chains through the dismiss callback.
- PBKDF2 runs on `Dispatchers.Default`, off the main thread. Sign-in pays it
  once per stored blob, because the username is inside the ciphertext and there
  is nothing public to look up on.

## Files

- `app/src/main/java/org/pgo/minimal/`
  - `Crypto.kt` — PBKDF2, AES-256-GCM, P-256 keygen, hex.
  - `Vault.kt` — records, the plaintext and `[group1]` format, sign-in, merge rules.
  - `Roster.kt` — the parser and rewriter, ported with the original regexes intact.
  - `MainActivity.kt` — the screen, its two views, and all the wiring.
- `app/src/main/res/` — layouts, the light and dark palettes, styles.
- `build.gradle.kts`, `settings.gradle.kts` — AGP 8.5.2, Kotlin 1.9.24,
  compileSdk 34, minSdk 26, matching `prototype-apk`.

## Still to do, once it compiles

- A first build, and whatever it turns up.
- The differential test the WASM prototype has: run the roster port against
  prototype-minimal's own JavaScript on the same inputs. That found two real
  bugs there and would be worth having here — it needs a JVM to run.
- A vault round trip against the browser, done for real rather than reasoned
  about.
