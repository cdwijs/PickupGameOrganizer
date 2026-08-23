# prototype-minimal-apk

`prototype-minimal` as a native Android app. Decided with Cedric: a Kotlin
rewrite on the platform's own APIs, not a WebView around the existing HTML.

Named `prototype-minimal-apk` because **`prototype-apk` is already taken** — it
is the Android port of `prototype-arti` (`org.pgo.artip2p`, Kotlin over a Rust
JNI core), committed in `9659138`.

## Status: built and signed

`gradle assembleDebug` in the new `claude-android` image produces a 5.7 MB
`app-debug.apk` — package `org.pgo.minimal`, minSdk 26 / targetSdk 34,
launchable `MainActivity`, signed under APK Signature Scheme v2 with the debug
certificate. It compiled first time. **It has not been run on a device.**

Getting there needed three pre-existing problems in claude-code-docker sorted
out first; see *Toolchain* below.

## How it started: written, not built

The container has no JDK, no Android SDK, no Gradle and no NDK — `java` does
not exist anywhere on the filesystem. An APK is binary XML plus DEX bytecode,
so unlike the WebAssembly prototype there is no hand-rolling it with what is
here.

Per the container rules the toolchain belongs in the image, not in an ad-hoc
install. Cedric chose to add it and have the build and verification happen
here afterwards. The project was written in the meantime, then built once the
image existed.

## Toolchain

`Dockerfile.android` was added to claude-code-docker: the Android half of
`Dockerfile.flutter` without Flutter/Dart, stacked on
`claude-code-agent-jvm-base`. SDK platforms 34 & 36, build-tools 34.0.0 &
36.0.0, Gradle 8.7, no NDK (bind-mounted from `android-sdk-cache/ndk` when a
project needs one). Wired into `docker-compose.yml`, `build.sh`, `update.sh`,
`gen-webui-config.sh` and the `ccd`/`ccg`/`ccq` launchers, and documented in
that repo's README.

Three problems surfaced on the way, none of them Android-specific:

1. **The disk was full** — 3.0 GB free of 932 GB. `docker builder prune`
   returned enough to continue.
2. **`install-claude.sh` was not idempotent**, despite its own header saying
   "install (or update)": `ln -s` with no `-f`, and a `mv` that would nest a
   new install inside an existing one. Every overlay ending in that layer fails
   on a base that already has Claude. Fixed in place.
3. **The local `claude-code-agent-base` was built without `--target base`**, so
   it carries a Claude install (2.1.148, May) — which is what tripped over the
   bug above. Rebuilding it properly turned out to be impossible right now: the
   base Dockerfile patches `@cloudcli-ai/cloudcli` with `sed`, and the current
   package no longer ships `dist-server/server/claude-sdk.js`, so the build dies
   at that step. **That breakage predates this work and still stands** — it
   blocks `claude-flutter` too.

Because of (3), `Dockerfile.android` ships its own copy of `install-claude.sh`
over the parent's, which keeps it buildable on whatever base is to hand and
becomes a no-op once the base is fixed.

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

## Left

1. **Run the APK.** It assembles and signs; nothing has launched it. The layout
   is the least proven part — styles supplying `layout_width`, and two
   `<include>`s of one card layout binding separately.
2. The differential test the WASM port has — the roster port against
   prototype-minimal's own JavaScript, on the same inputs. It found two real
   bugs there, and there is a JVM to run it on now.
3. A vault round trip against the browser, run rather than reasoned about.
