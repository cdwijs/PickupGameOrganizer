# prototype-minimal-apk

`prototype-minimal` as a native Android app: Kotlin UI, the platform's crypto,
and the same vault format, so a vault moves between this and the web and
WebAssembly prototypes.

## Build status

Built and signed. `gradle assembleDebug` in the `claude-android` image
produces `app/build/outputs/apk/debug/app-debug.apk`:

| | |
|---|---|
| package | `org.pgo.minimal`, versionCode 1, versionName 0.1 |
| size | 5.7 MB |
| SDK | minSdk 26, targetSdk 34, compileSdk 34 |
| activity | `org.pgo.minimal.MainActivity` |
| signature | APK Signature Scheme v2, debug certificate |

It compiled first time, which is luck as much as care — it was written before
the toolchain existed and had never been near a compiler. **It has not been run
on a device or an emulator**, so the build is proof that it assembles, not that
it behaves. The layout in particular is unproven: `style="@style/Panel"`
supplying `layout_width`/`layout_height`, and two `<include>`s of the same card
layout resolving to separate view bindings, both work in theory.

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

Use the **`claude-android`** image (`Dockerfile.android` in the
claude-code-docker repo): Temurin JDK 21, Android SDK with platforms 34 & 36,
build-tools 34.0.0 & 36.0.0, and Gradle 8.7. Point the workspace at it in
`workspaces.conf`:

```
game  /home/gaming/git-werkmap/PickupGameOrganizer/  claude-android
```

Then, from this directory:

```sh
gradle --no-daemon assembleDebug
# app/build/outputs/apk/debug/app-debug.apk
```

`gradle-wrapper.jar` is not committed (the sibling project does the same), so
`./gradlew` needs it restoring first — `gradle wrapper` regenerates it. Using
the system Gradle directly, as above, skips that entirely.

Gradle's cache and the debug keystore live under `$HOME`, which is a mounted
volume, so dependencies download once and every debug APK keeps the same
signing key.

<details>
<summary>What was added to the image</summary>

`Dockerfile.android` in the claude-code-docker repo — the Android half of
`Dockerfile.flutter` without Flutter/Dart, stacked on `claude-code-agent-jvm-base`
for the JDK. Android SDK cmdline-tools, platform-tools, platforms 34 & 36,
build-tools 34.0.0 & 36.0.0, and Gradle 8.7, with a build-time check that
`aapt2`, `d8` and `apksigner` are all present.

The NDK is deliberately not baked in — it is ~5 GB and only JNI projects need
it. `/opt/android-sdk/ndk` is bind-mounted from `./android-sdk-cache/ndk`, so
`sdkmanager "ndk;<version>"` inside a session persists across image rebuilds.
There is no native code here, unlike `prototype-apk`.

</details>

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

## Still to do

- **Run it.** The APK assembles and signs; nothing has launched it. Installing
  on a device is the next real test.
- The differential test the WASM prototype has: run the roster port against
  prototype-minimal's own JavaScript on the same inputs. That found two real
  bugs there, and there is a JVM to run it on now.
- A vault round trip against the browser, done for real rather than reasoned
  about.
