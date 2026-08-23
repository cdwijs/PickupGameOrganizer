# prototype-minimal-apk

`prototype-minimal` as a native Android app. Decided with Cedric: a Kotlin
rewrite on the platform's own APIs, not a WebView around the existing HTML.

Named `prototype-minimal-apk` because **`prototype-apk` is already taken** — it
is the Android port of `prototype-arti` (`org.pgo.artip2p`, Kotlin over a Rust
JNI core), committed in `9659138`.

## Status: written, not built

The container has no JDK, no Android SDK, no Gradle and no NDK — `java` does
not exist anywhere on the filesystem. An APK is binary XML plus DEX bytecode,
so unlike the WebAssembly prototype there is no hand-rolling it with what is
here.

Per the container rules the toolchain belongs in the image, not in an ad-hoc
install. Cedric chose to add it and have the build and verification happen
here afterwards. The project was written in the meantime so it is ready to
build the moment the image has the SDK; **nothing in it has been compiled.**

What the image needs (no NDK — there is no native code here):

- `openjdk-17-jdk-headless`
- Android command line tools, then `sdkmanager "platforms;android-34"
  "build-tools;34.0.0" "platform-tools"`

The exact Dockerfile block is in the prototype's README.

## Shape

| | prototype-minimal | here |
|---|---|---|
| storage | `localStorage` | `SharedPreferences` |
| crypto | WebCrypto | `javax.crypto` / `java.security` |
| clipboard | async Clipboard API | `ClipboardManager` |
| autofill | Credential Management API | Android autofill |
| UI | HTML + CSS | Kotlin + XML |

Four Kotlin files: `Crypto.kt`, `Vault.kt`, `Roster.kt`, `MainActivity.kt`.
Versions match `prototype-apk` — AGP 8.5.2, Kotlin 1.9.24, compileSdk 34,
minSdk 26 (PBKDF2WithHmacSHA256 and `java.time` both need 26).

## Decisions

- **PBKDF2 over `Mac("HmacSHA256")`, not `SecretKeyFactory`/`PBEKeySpec`.**
  `PBEKeySpec` takes a `CharArray` and providers have historically differed on
  how those become bytes; WebCrypto is unambiguously UTF-8. Deriving over
  `password.toByteArray(UTF_8)` keeps the vault interoperable, including for
  non-ASCII passwords, for about twenty lines of code.
- **The vault format is unchanged**, so a vault pastes between this app and the
  browser. That rules out the Android Keystore, which would be the right answer
  for a real app and would also make the blobs non-portable.
- **No silent re-unlock.** Android autofill fills a form the user is looking
  at; it cannot be read programmatically the way the Credential Management API
  can. A restarted session therefore shows *locked* plus the Unlock button.

## Self-review found, before any compiler did

- The manifest pointed at `@mipmap/ic_launcher`, which does not exist — a
  missing resource fails the build. Replaced with a vector drawable.
- The sign-in coroutine returned `Any?` because of a sentinel value, forcing a
  cast. Checking for the deleted blob before launching keeps it `Unlocked?`.
- `Theme.Material3.DayNight` with a light-only palette would have put light
  system text on white panels in dark mode. Added `values-night/colors.xml`.
- `GameCardBinding` was referenced fully qualified inline.

None of that is a substitute for a build.

## Left for when it compiles

1. `./gradlew assembleDebug`, and whatever the first build turns up.
2. The differential test the WASM port has — the roster port against
   prototype-minimal's own JavaScript, on the same inputs. It found two real
   bugs there. Needs a JVM.
3. A vault round trip against the browser, run rather than reasoned about.
