# Arti P2P — Android prototype

An Android port of `prototype-arti`: the **same Arti onion-service networking
core** (Rust) compiled to a native `.so` and driven by a thin Kotlin UI.

Same behavior as the desktop version:
- **Connect** with both fields empty → bootstrap Tor, host an **onion service**,
  and fill the fields with this node's **onion address** + **/onion3 multiaddr**.
- **Connect** with a field filled → dial that peer over Tor.
- **Status area** → live status + `⟵ received:` data.
- **Send increasing number every second** → streams `1, 2, 3, …` to the peer.

### Full-address visibility
The onion address (62 chars) and multiaddr are shown in **multiline, monospace,
selectable** fields (`inputType=textMultiLine`, no `singleLine`/ellipsize), so
the complete address always **wraps and stays fully visible in portrait** — and
is long-press-selectable to copy.

## Architecture

```
Kotlin UI (MainActivity)                Rust core (libarti_p2p_android.so)
  Connect / Counter buttons  --JNI-->     nativeConnect / nativeStartCounter
  poll every 200ms           <--JNI--     nativePoll() drains an event queue
```
- `app/` — Android app (Kotlin, single activity).
- `rust/` — `cdylib` JNI crate; reuses the desktop onion-service logic, emitting
  `LOG` / `RECV` / `ONION` / `MADDR` events instead of egui updates.
- Data channel is onion-only (length-prefixed stream); no libp2p yet.

## Prerequisites

- **JDK 17**, **Android SDK** (compileSdk 34), **Android NDK** (r26+).
- **Rust** + Android targets and **cargo-ndk**:
  ```bash
  rustup target add aarch64-linux-android x86_64-linux-android
  cargo install cargo-ndk
  ```
- `ANDROID_NDK_HOME` set (or NDK installed under `$ANDROID_HOME/ndk/<ver>`).

## Build

**1. Build the Rust native library into `jniLibs`** (arm64 for devices, x86_64 for
the emulator):
```bash
cd prototype-apk/rust
cargo ndk -t arm64-v8a -t x86_64 -o ../app/src/main/jniLibs build --release
```
This produces `app/src/main/jniLibs/{arm64-v8a,x86_64}/libarti_p2p_android.so`.

**2. Build the APK.** The Gradle wrapper jar isn't committed — generate it once
(or just open the project in Android Studio, which does it for you):
```bash
cd prototype-apk
gradle wrapper --gradle-version 8.7   # first time only
./gradlew assembleDebug
```
APK: `app/build/outputs/apk/debug/app-debug.apk`

**3. Install & run:**
```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## Testing with two nodes

Onion services always route over the real Tor network, so the two peers can be
any two devices/emulators with internet:

1. Device A: leave fields empty → **Connect**. Wait for “Onion service PUBLISHED”
   (30 s–2 min), then copy the onion address shown.
2. Device B: paste A's onion address → **Connect**.
3. Press **Send increasing number every second** on either device; watch the
   numbers arrive in the other's status area.

## Known limitations

- Arti onion services are officially **experimental**.
- Single active peer connection; last connection wins in host mode.
- No persistent onion identity yet (new address per keystore state).
- Bootstrap/publish is slower on mobile networks; be patient on first run.
- Networking runs in-process tied to the activity; a production app would use a
  foreground `Service` so Tor survives backgrounding.
