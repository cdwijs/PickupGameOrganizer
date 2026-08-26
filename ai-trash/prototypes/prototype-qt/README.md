# prototype-qt

One button and one textbox counting its presses, in C++/Qt 6, built from the
same ~90 lines of C++ into three things:

| target | output | what it is |
|---|---|---|
| PWA | `dist/pwa/` (14 MB) | WebAssembly, installable, works offline |
| Linux | `dist/counter` (33 kB) | native x86_64 ELF |
| Android | `dist/counter-debug.apk` (15 MB) | `org.pgo.qtcounter`, arm64-v8a |

The point of the prototype is that column: what one Qt source tree costs and
how much of the three-platform story a single `./build.sh` can actually carry.
The app itself is deliberately trivial, so anything that goes wrong is the
toolchain's doing and not the code's.

## Build status

All three built and the behaviour checked, on 2026-08-26:

- **PWA** — served and driven in headless Chromium 148: the page loads, Qt
  paints, three synthetic clicks on *Press me* leave `3` in the textbox, and
  the service worker registers.
- **Linux** — builds against Qt 6.7.3 and runs; `ctest` drives the widget with
  synthetic clicks under the offscreen platform (3 test functions, passing).
- **Android** — `apk` target produces a debug-signed (APK Signature Scheme v2)
  15 MB APK for arm64-v8a, `minSdk 28 / targetSdk 34`. **Not installed on a
  device or an emulator** — it assembles and it is signed, which is not the
  same as saying it runs.

## The app

`src/counterwindow.h` is the entire application: a `QWidget` with a `QLabel`
caption, a read-only `QLineEdit` showing the count, and a `QPushButton` that
increments it. `src/main.cpp` shows it. Nothing is per-platform.

Qt **Widgets**, not QML. Widgets needs only `qtbase`, which is the one module
guaranteed to be in the wasm and Android packages aqt installs; QML would add a
runtime, a second language and `qtdeclarative` to every target for a button and
a text field.

The textbox is `setReadOnly(true)` and `Qt::NoFocus`: it is a display, and on
Android focusing it would raise the soft keyboard over an app that has nothing
to type into.

## Building

The build needs Qt 6 for three targets, Emscripten and the Android
SDK/NDK — none of which are in the default agent image. They are all in
**`claude-code-agent-qt`** (`Dockerfile.qt` in the claude-code-docker repo).

Either point the workspace at that image in `workspaces.conf`:

```
game  /home/gaming/git-werkmap/PickupGameOrganizer/  claude-qt
```

and build in the session:

```sh
./build.sh                 # linux + tests + pwa + android
./build.sh pwa             # or any subset
```

or, from a session on the ordinary image, let the script drive the Qt image
over the mounted Docker socket:

```sh
./build.sh --docker        # same arguments, run inside claude-code-agent-qt
```

`--docker` gives the build container this workspace's own bind mounts (host
paths come out of the running container's mount table, since `/workspace` means
nothing to the daemon), then hands the results back with `chown`.

Nothing is built into the source tree. Everything lands under `$BUILD_DIR`,
default `/shared/tmp/prototype-qt`:

```
/shared/tmp/prototype-qt/dist/counter              native Linux executable
/shared/tmp/prototype-qt/dist/pwa/                 serve this folder
/shared/tmp/prototype-qt/dist/counter-debug.apk    adb install this
/shared/tmp/prototype-qt/{linux,wasm,android}/     the three build trees
/shared/tmp/prototype-qt/gradle-home/              gradle's downloads
/shared/tmp/prototype-qt/android-home/             the debug signing key
```

The last two are kept out of `$HOME` on purpose: Gradle then downloads its
distribution once rather than once per container, and the debug APK keeps the
same signing key, so a rebuild installs over the copy already on the phone
instead of being refused for a signature mismatch.

### Running what it produced

```sh
# PWA — a plain static server is enough; localhost counts as a secure context,
# so the service worker registers and the app is installable.
cd /shared/tmp/prototype-qt/dist/pwa && python3 -m http.server 8080

# Linux
/shared/tmp/prototype-qt/dist/counter

# Android
adb install -r /shared/tmp/prototype-qt/dist/counter-debug.apk
```

For a phone on the LAN the PWA needs HTTPS — same story as the other
prototypes here, and
[`prototype-minimal/README.md`](../prototype-minimal/README.md) has the
certificate recipe.

## Emscripten has to match Qt exactly

The one real trap, and the reason `build.sh` refuses to build rather than
warning. Qt 6.7.3 for WebAssembly is built against **Emscripten 3.1.50**. The
image ships 3.1.56, and with that combination the app:

- compiles with no error,
- links with no error,
- loads in the browser, creates its canvas at the right size,
- and never paints. The page is blank grey. The console holds a wall of
  `QPainter::begin: Paint device returned engine == 0` — nothing that points at
  the toolchain.

`build.sh` reads the version Qt wants out of
`$QT_WASM/lib/cmake/Qt6/QtPublicWasmToolchainHelpers.cmake`, looks for an emsdk
that has it (`$EMSDK_DIR`, then `/shared/tmp/emsdk-<version>`, then
`/opt/emsdk`), and stops with an explanation if it can only find another one.
`ALLOW_EMSDK_MISMATCH=1` overrides that, for the day a newer Qt is relaxed
about it.

There is a matching emsdk at `/shared/tmp/emsdk-3.1.50` (1.3 GB), which is what
the working build used and what the script finds on its own. The real fix is
one line in `Dockerfile.qt` — `ARG EMSDK_VERSION=3.1.50`, the version its own
comment claims it pins — after which that copy can be deleted.

## Why 14 MB

Measured on the shipped build (`wasm-names`, `wasm-oz` and `qt-size-probe`
under `/shared/tmp/prototype-qt/` are the throwaway builds behind these
numbers):

| | |
|---|---|
| `counter.wasm` | 13.8 MB — 10.4 MB code, 3.3 MB data |
| over the wire, gzipped | 5.2 MB |
| functions | 19,510 |
| this app's own code | ~18 kB, 0.2% of it |

Where the code comes from, by the static archive each of the 19,510 functions
was linked out of:

| | |
|---|---|
| QtGui | 29% |
| QtWidgets | 26% |
| QtCore | 22% |
| bundled harfbuzz, freetype, libjpeg, libpng, pcre2, zlib, QtSvg | 17% |
| libc++, emscripten, the app | 6% |

**It is not compiler settings.** `MinSizeRel` (`-Os`) instead of `Release`
(`-O2`) gives 13.58 MB — 1.3% off. The size is decided by how much of Qt the
link graph reaches, not by how tightly it is compiled.

**It is not this app, and it is not Widgets alone.** Qt for wasm has a floor,
and it is high: `QCoreApplication` plus one `qDebug()` is 2.5 MB, and
`QGuiApplication` plus a `QWindow` is 8.9 MB. Widgets adds the last 4.9 MB.

**It is everything a `QApplication` can reach.** Dead-code elimination happens
at function level, and a function stays if anything reaches it — so linked into
this two-widget app are `QFileDialog`, `QGraphicsScene`, `QXmlStreamReader`,
the HTML importer and exporter for `QTextDocument`, `QStyleSheetStyle`, the RHI
OpenGL backend and its shader compiler, the JPEG *encoder*, and harfbuzz's full
shaping machinery including the Indic and Universal Shaping Engine syllable
tables. A `QApplication` constructs a style, `QFusionStyle` and `QCommonStyle`
draw every widget class there is, the text stack pulls harfbuzz and freetype
and the whole document model, and the image formats are registered in both
directions. Nothing here is reachable *by this program*, but all of it is
reachable *in the graph*.

The 3.3 MB data section is the same story in constants: the bundled DejaVu
font, Unicode and locale and currency tables, freetype's glyph-name tables, the
Qt logo and the *About Qt* text.

So the levers, in order of what they actually buy:

- Serve it compressed. 13.8 MB becomes 5.2 MB gzipped for one line of server
  config, and this is the only lever that costs nothing.
- Build Qt from source with the features cut (`-no-feature-…`, the "Qt Lite"
  route). This is the only way to get at the 75% of the binary that is Qt
  subsystems this app never calls, and it means owning a Qt build.
- Don't put Qt Widgets on the web. `prototype-webassembly` is a bigger app than
  this one in 47 kB of hand-written C — a factor of 300. Qt's value here is the
  Linux and Android targets sharing the source; the PWA is the target where the
  bill comes due.

## Tests

```sh
./build.sh test            # or --docker test
```

`tests/tst_counter.cpp` is QTest against the real `CounterWindow` under
`QT_QPA_PLATFORM=offscreen`: the textbox starts at `0`, three synthetic clicks
walk it to `3`, and typing into the textbox cannot change the count. It runs on
the host build only — the behaviour under test is the same code the wasm and
Android builds package.

## Files

- `src/counterwindow.h` — the widget: caption, textbox, button, the count.
- `src/main.cpp` — `QApplication`, show, exec.
- `tests/tst_counter.cpp` — the click test.
- `CMakeLists.txt` — one `qt_add_executable`, plus the Android packaging
  properties and the wasm heap size.
- `build.sh` — toolchain discovery, the three builds, the PWA assembly, and
  `--docker`.
- `android/AndroidManifest.xml` — Qt's own template with the package renamed to
  `org.pgo.qtcounter`. androiddeployqt copies its template tree first and this
  directory over the top, so this is the only Android file needed. The
  `-- %%INSERT_…%% --` placeholders are filled in from the `QT_ANDROID_*`
  properties in `CMakeLists.txt`; note that they contain `--`, which XML does
  not allow inside a comment, so nothing near them can be commented out.
- `web/manifest.json`, `web/sw.js`, `web/icon.svg` — the PWA parts Qt does not
  generate.
- `web/pwa-head.html` — injected into Qt's generated shell by `build.sh`.

Qt's wasm build emits `counter.html`, `counter.js`, `counter.wasm` and
`qtloader.js`. `build.sh` patches the HTML rather than replacing it — the
manifest link, the icon, the theme colour and the service-worker registration
go in before `</head>`, and it is written out as `index.html`. Hand-writing the
loader glue would mean re-deriving it every time Qt changes its shell template,
which it has done twice in the 6.x series.

## Notes and limitations

- Nothing generated is committed. `prototype-webassembly` commits its 47 kB
  `app.wasm` so a fresh checkout can be served immediately; the equivalent here
  is 14 MB, which is a build artifact and not a web asset.
- The Linux executable links against the Qt 6.7.3 in `/opt/qt`, so it runs in
  the Qt image but not on a bare host. `QT_HOST_LINUX= QT_ROOT= ./build.sh
  linux` builds against Debian's Qt 6.4 instead; a genuinely portable binary
  needs a static Qt or `linuxdeploy`, neither of which this prototype does.
- The APK is arm64-v8a only, because that is the only Android Qt the image
  installs; another ABI means `aqt install-qt linux android 6.7.3
  android_armv7` and `$QT_ANDROID` pointed at it, plus `ANDROID_ABI` on the
  build. It is debug-signed, which is enough for `adb
  install` and no use for a store: a release APK needs `androiddeployqt
  --release --sign <keystore> <alias>`, and the keystore is not in the repo.
- `Dockerfile.qt` also carries an MXE toolchain for a static Windows `.exe`.
  `build.sh` has no `windows` target; adding one is a fourth call to `cmake`
  with `$MXE_ROOT/usr/$MXE_TARGET/qt6/lib/cmake` in `CMAKE_PREFIX_PATH`.
- The image's `QT_HOST_LINUX` points at `linux_gcc_64`, which does not exist —
  the Qt in it is at `gcc_64`. `build.sh` looks for the host Qt rather than
  trusting the variable, so both spellings work.
