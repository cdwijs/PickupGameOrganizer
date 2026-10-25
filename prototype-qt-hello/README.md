# Qt Hello prototype

A minimal Qt Widgets app (a window with a greeting label) used to verify the
Qt cross-build matrix of the `claude-qt` dev image.

| Target        | Qt   | Output                                   |
|---------------|------|------------------------------------------|
| Linux desktop | 6.8  | `src/QtHello` (ELF x86-64)               |
| Windows 64    | 5.15 | `src/QtHello.exe` (PE32+, MXE mingw-w64) |
| Windows 32    | 5.15 | `src/QtHello.exe` (PE32, MXE i686)       |
| Android       | 6.8  | `src/android-build/QtHello.apk` (arm64)  |
| WebAssembly   | 6.8  | `src/QtHello.{html,js,wasm}`             |

The CMake files accept both Qt 6 and Qt 5.15, because the MXE Windows
toolchain only ships Qt 5.

## Build

Use the `qt-build` helper from the dev image. Put build directories outside
the source tree:

```bash
cd prototype-qt-hello
qt-build desktop   . /shared/tmp/qt-hello/build-desktop
qt-build windows   . /shared/tmp/qt-hello/build-windows
qt-build windows32 . /shared/tmp/qt-hello/build-windows32
qt-build android   . /shared/tmp/qt-hello/build-android --apk
qt-build wasm      . /shared/tmp/qt-hello/build-wasm
```

Unit tests (desktop only):

```bash
ctest --test-dir /shared/tmp/qt-hello/build-desktop --output-on-failure
```

## Running

- **Windows:** the `.exe` needs the Qt 5 / mingw DLLs from `$MXE_ROOT`
  (`usr/<target>/bin` and `usr/<target>/qt5/plugins/platforms`) next to it.
- **Android:** the APK is unsigned. Sign it, or build a debug APK, before
  `adb install`.
- **WebAssembly:** serve the build directory over HTTP (for example
  `python3 -m http.server`) and open `QtHello.html`.
