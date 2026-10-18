# prototype-tor-qt: a Qt app with its own embedded Tor node (libtor) and an
# in-process C++ Snowflake client, for native Linux, Android (arm64-v8a) and
# the browser (Qt for WebAssembly with threads, wasm_multithread).
#
# Build the native dependencies first (once per target):
#   deps/build-deps.sh linux      -> deps/install/linux
#   deps/build-deps.sh android    -> deps/install/android
#   deps/build-deps.sh wasm       -> deps/install/wasm
# Override the location with: qmake DEPS_PREFIX=/path/to/install/<target>

TEMPLATE = app
TARGET = prototype-tor-qt
QT += widgets network
CONFIG += c++17 warn_on

wasm {
    DEPS_TARGET = wasm
    !qtConfig(thread): error("the browser build needs Qt for WebAssembly with threads (wasm_multithread)")
} else:android {
    DEPS_TARGET = android
    !equals(QT_ARCH, arm64-v8a): error("deps/build-deps.sh android builds arm64-v8a only (QT_ARCH is $$QT_ARCH)")
} else:linux {
    DEPS_TARGET = linux
} else {
    error("only Linux, Android and WebAssembly are supported")
}
isEmpty(DEPS_PREFIX): DEPS_PREFIX = $$PWD/deps/install/$$DEPS_TARGET
!exists($$DEPS_PREFIX/lib/libtor.a): error("missing $$DEPS_PREFIX/lib/libtor.a - run deps/build-deps.sh $$DEPS_TARGET first")

INCLUDEPATH += \
    $$PWD/src \
    $$PWD/src/third_party/kcp \
    $$DEPS_PREFIX/include

SOURCES += \
    src/main.cpp \
    src/appvisibility.cpp \
    src/mainwindow.cpp \
    src/torcontroller.cpp \
    src/chatlink.cpp \
    src/snowflake/broker.cpp \
    src/snowflake/client.cpp \
    src/snowflake/encapsulation.cpp \
    src/snowflake/session.cpp \
    src/snowflake/webrtcpeer.cpp \
    src/third_party/kcp/ikcp.c

HEADERS += \
    src/appvisibility.h \
    src/mainwindow.h \
    src/torcontroller.h \
    src/chatlink.h \
    src/loopback.h \
    src/snowflake/broker.h \
    src/snowflake/client.h \
    src/snowflake/encapsulation.h \
    src/snowflake/peer.h \
    src/snowflake/session.h \
    src/snowflake/webrtcpeer.h \
    src/third_party/kcp/ikcp.h

wasm {
    # netshim replaces libc's socket/fd/poll calls with in-tab loopback pipes;
    # ShimSocket/ShimServer are the Qt side of it.
    SOURCES += \
        src/wasm/netshim.c \
        src/wasm/shimsocket.cpp \
        src/snowflake/websocketpeer.cpp
    HEADERS += \
        src/wasm/netshim.h \
        src/wasm/shimsocket.h \
        src/snowflake/websocketpeer.h
}

RESOURCES += resources.qrc

# Vendored KCP is the only C source; keep its upstream style warnings quiet.
QMAKE_CFLAGS_WARN_ON += -Wno-unused-parameter

# Static archives, in dependency order (tor and libdatachannel both need OpenSSL).
LIBS += $$DEPS_PREFIX/lib/libtor.a
wasm {
    # The browser's RTCPeerConnection and WebSocket, behind libdatachannel's rtc:: API.
    LIBS += $$DEPS_PREFIX/lib/libdatachannel-wasm.a
} else {
    LIBS += \
        $$DEPS_PREFIX/lib/libdatachannel.a \
        $$DEPS_PREFIX/lib/libjuice.a \
        $$DEPS_PREFIX/lib/libusrsctp.a
}
LIBS += \
    $$DEPS_PREFIX/lib/libevent.a \
    $$DEPS_PREFIX/lib/libz.a

wasm {
    LIBS += $$DEPS_PREFIX/lib/libssl.a $$DEPS_PREFIX/lib/libcrypto.a
    QMAKE_LFLAGS += \
        --js-library $$DEPS_PREFIX/lib/js/webrtc.js \
        --js-library $$DEPS_PREFIX/lib/js/websocket.js \
        -lidbfs.js \
        -sDEFAULT_PTHREAD_STACK_SIZE=1MB  # tor needs more than the 64 KB default
    # WebRtcPeer catches datachannel exceptions; Emscripten aborts on throw otherwise.
    QMAKE_CXXFLAGS += -fexceptions
    QMAKE_LFLAGS += -fexceptions
    # Read by Qt's wasm.prf. tor, its cpuworker and Qt's own threads all come
    # from the pthread pool. iOS Safari needs a modest fixed memory maximum
    # for shared (threaded) memory.
    QT_WASM_PTHREAD_POOL_SIZE = 8
    isEmpty(QT_WASM_INITIAL_MEMORY): QT_WASM_INITIAL_MEMORY = 96MB
    isEmpty(QT_WASM_MAXIMUM_MEMORY): QT_WASM_MAXIMUM_MEMORY = 256MB
    # web/index.html sets environment variables and mounts IndexedDB in preRun.
    QT_WASM_EXTRA_EXPORTED_METHODS = ENV,addRunDependency,removeRunDependency
    # Our own page, service worker and manifest next to the generated files.
    QMAKE_POST_LINK += $$QMAKE_COPY_DIR $$shell_path($$PWD/web/.) $$shell_path($$OUT_PWD)
} else:android {
    # OpenSSL is shared on Android: Qt's TLS backend (used for the Snowflake
    # broker request) dlopens libssl_3.so/libcrypto_3.so, and tor and
    # libdatachannel link the very same copies.
    LIBS += -L$$DEPS_PREFIX/lib -lssl -lcrypto -llog
    ANDROID_EXTRA_LIBS += \
        $$DEPS_PREFIX/lib/libcrypto_3.so \
        $$DEPS_PREFIX/lib/libssl_3.so
    ANDROID_MIN_SDK_VERSION = 28
    ANDROID_TARGET_SDK_VERSION = 34
} else {
    LIBS += $$DEPS_PREFIX/lib/libssl.a $$DEPS_PREFIX/lib/libcrypto.a -lpthread -ldl
}
LIBS += -lm
