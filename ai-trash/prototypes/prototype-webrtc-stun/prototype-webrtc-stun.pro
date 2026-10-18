# prototype-webrtc-stun: two nodes chat over a direct WebRTC data channel,
# found by node name through public ntfy servers, with a random STUN server
# per attempt and no TURN. Native Linux, Android (arm64-v8a) and the browser
# (Qt for WebAssembly, single-threaded).
#
# Build the native dependencies first (once per target):
#   deps/build-deps.sh linux      -> deps/install/linux
#   deps/build-deps.sh android    -> deps/install/android
#   deps/build-deps.sh wasm       -> deps/install/wasm
# Override the location with: qmake DEPS_PREFIX=/path/to/install/<target>

TEMPLATE = app
TARGET = prototype-webrtc-stun
QT += widgets network
CONFIG += c++17 warn_on

wasm {
    DEPS_TARGET = wasm
    DEPS_CHECK = libdatachannel-wasm.a
    qtConfig(thread): error("deps/build-deps.sh wasm builds without threads; use Qt wasm_singlethread")
} else:android {
    DEPS_TARGET = android
    DEPS_CHECK = libdatachannel.a
    !equals(QT_ARCH, arm64-v8a): error("deps/build-deps.sh android builds arm64-v8a only (QT_ARCH is $$QT_ARCH)")
} else:linux {
    DEPS_TARGET = linux
    DEPS_CHECK = libdatachannel.a
} else {
    error("only Linux, Android and WebAssembly are supported")
}
isEmpty(DEPS_PREFIX): DEPS_PREFIX = $$PWD/deps/install/$$DEPS_TARGET
!exists($$DEPS_PREFIX/lib/$$DEPS_CHECK): error("missing $$DEPS_PREFIX/lib/$$DEPS_CHECK - run deps/build-deps.sh $$DEPS_TARGET first")

INCLUDEPATH += \
    $$PWD/src \
    $$DEPS_PREFIX/include

SOURCES += \
    src/main.cpp \
    src/mainwindow.cpp \
    src/connector.cpp \
    src/nodename.cpp \
    src/peerlink.cpp \
    src/signaling.cpp \
    src/stunlist.cpp

HEADERS += \
    src/mainwindow.h \
    src/connector.h \
    src/nodename.h \
    src/peerlink.h \
    src/signaling.h \
    src/stunlist.h

RESOURCES += resources.qrc

wasm {
    # The browser's RTCPeerConnection and WebSocket, behind libdatachannel's rtc:: API.
    LIBS += $$DEPS_PREFIX/lib/libdatachannel-wasm.a
    QMAKE_LFLAGS += \
        --js-library $$DEPS_PREFIX/lib/js/webrtc.js \
        --js-library $$DEPS_PREFIX/lib/js/websocket.js
    # PeerLink catches datachannel exceptions; Emscripten aborts on throw otherwise.
    QMAKE_CXXFLAGS += -fexceptions
    QMAKE_LFLAGS += -fexceptions
    # web/index.html passes URL query parameters in as environment variables.
    QT_WASM_EXTRA_EXPORTED_METHODS = ENV
    # Our own page and icon next to the generated files.
    QMAKE_POST_LINK += $$QMAKE_COPY_DIR $$shell_path($$PWD/web/.) $$shell_path($$OUT_PWD)
} else:android {
    # libdatachannel gets its own static, hidden OpenSSL. The shared copies are
    # only for Qt's TLS backend (ntfy HTTPS), which dlopens libssl_3.so and
    # libcrypto_3.so at runtime. The app must not link against them: a Qt kit
    # that deploys its own OpenSSL under the same names (Arch's AUR cross Qt)
    # may overwrite ours in the APK, and the app then failed to load.
    LIBS += \
        $$DEPS_PREFIX/lib/libdatachannel.a \
        $$DEPS_PREFIX/lib/libjuice.a \
        $$DEPS_PREFIX/lib/libusrsctp.a \
        $$DEPS_PREFIX/lib/libssl.a \
        $$DEPS_PREFIX/lib/libcrypto.a \
        -Wl,--exclude-libs,libssl.a:libcrypto.a \
        -llog
    ANDROID_EXTRA_LIBS += \
        $$DEPS_PREFIX/lib/libcrypto_3.so \
        $$DEPS_PREFIX/lib/libssl_3.so
    # The deps are built against one API level (deps/install/android/android-api,
    # default 28): usrsctp calls getrandom() and libjuice uses stdout, which
    # older libc stubs lack. A kit whose default ANDROID_PLATFORM is lower
    # (e.g. a distribution's cross Qt) would fail to link, so raise its
    # -target triple to that level; the app needs API 28 anyway.
    DEPS_ANDROID_API = $$cat($$DEPS_PREFIX/android-api)
    isEmpty(DEPS_ANDROID_API): DEPS_ANDROID_API = 28
    KIT_ANDROID_API = $$replace(ANDROID_PLATFORM, "android-", "")
    lessThan(KIT_ANDROID_API, $$DEPS_ANDROID_API) {
        message("Qt kit targets $$ANDROID_PLATFORM; building for android-$$DEPS_ANDROID_API to match deps/install/android")
        QMAKE_CFLAGS = $$replace(QMAKE_CFLAGS, -linux-android$${KIT_ANDROID_API}$, -linux-android$$DEPS_ANDROID_API)
        QMAKE_CXXFLAGS = $$replace(QMAKE_CXXFLAGS, -linux-android$${KIT_ANDROID_API}$, -linux-android$$DEPS_ANDROID_API)
        QMAKE_LINK = $$replace(QMAKE_LINK, -linux-android$${KIT_ANDROID_API}$, -linux-android$$DEPS_ANDROID_API)
    }
    ANDROID_MIN_SDK_VERSION = $$DEPS_ANDROID_API
    ANDROID_TARGET_SDK_VERSION = 34
} else {
    LIBS += \
        $$DEPS_PREFIX/lib/libdatachannel.a \
        $$DEPS_PREFIX/lib/libjuice.a \
        $$DEPS_PREFIX/lib/libusrsctp.a \
        $$DEPS_PREFIX/lib/libssl.a \
        $$DEPS_PREFIX/lib/libcrypto.a \
        -lpthread -ldl
}
