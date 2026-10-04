# One target, three platforms, and a core that is not written in C++.
#
# The vault, the crypto and the roster grammar come from the sibling prototype
# as C sources compiled into this binary. PROTO_HOSTED tells base.c to leave
# memcpy/memset/memmove to the libc that every one of these targets has, unlike
# the freestanding wasm build they were written for.
cmake_minimum_required(VERSION 3.21)

project(prototype-minimal-qt VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

set(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../prototype-webassembly/src")
set(CORE_SOURCES
    "${CORE_DIR}/aesgcm.c"
    "${CORE_DIR}/app.c"
    "${CORE_DIR}/base.c"
    "${CORE_DIR}/json.c"
    "${CORE_DIR}/p256.c"
    "${CORE_DIR}/roster.c"
    "${CORE_DIR}/sha256.c"
)
if(NOT EXISTS "${CORE_DIR}/app.c")
    message(FATAL_ERROR "the C core is missing: ${CORE_DIR}. "
                        "prototype-minimal-qt builds prototype-webassembly's sources; "
                        "check out the whole prototypes directory, not this folder alone.")
endif()

add_library(minimal-core STATIC ${CORE_SOURCES})
target_compile_definitions(minimal-core PRIVATE PROTO_HOSTED)
target_include_directories(minimal-core PUBLIC "${CORE_DIR}")
# export_name is a wasm attribute; every other target ignores it loudly.
target_compile_options(minimal-core PRIVATE -Wno-attributes)
# The Android package has to be a Debug build to stay debuggable — Qt passes
# --release to androiddeployqt for every other configuration, and a release
# APK is signed with a keystore this project does not have and cannot be
# reached with run-as or a debugger. Debug means -O0, and -O0 through 310 000
# PBKDF2 iterations is the difference between a sign-in that takes a second
# and one that takes half a minute on a phone. The core is straight-line
# crypto with nothing to step through, so it is optimised in every
# configuration; the C++ that a debugger is actually pointed at is not.
if(NOT MSVC)
    target_compile_options(minimal-core PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()

# QtKeychain, as a submodule pinned to a main-branch commit rather than to a
# release: keychain_wasm.cpp — the backend that drives the browser's own
# password manager, and the only reason all three targets can have a credential
# store from one dependency — landed after v0.14.0 and is in no tag yet.
set(QTKEYCHAIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qtkeychain")
if(NOT EXISTS "${QTKEYCHAIN_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "third_party/qtkeychain is empty — it is a git submodule. Run "
                        "`git submodule update --init "
                        "ai-trash/prototypes/prototype-minimal-qt/third_party/qtkeychain` "
                        "from the top of the repository.")
endif()
# Static, so the APK does not have to package a second .so and the PWA does not
# have to load one. Its own translations and tests are off: they would drag
# lupdate/lrelease and Qt6::Test into the Android and wasm toolchains for
# strings this app never shows.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TRANSLATIONS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
# libsecret is a build-time dependency of the Linux backend and is not in the
# Qt image; without it QtKeychain still reaches GNOME Keyring and KWallet over
# D-Bus, which is what a desktop session actually has running.
set(LIBSECRET_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory("${QTKEYCHAIN_DIR}" EXCLUDE_FROM_ALL)

set(APP_SOURCES
    src/main.cpp
    src/clipboard.cpp
    src/clipboard.h
    src/core.cpp
    src/core.h
    src/credentials.cpp
    src/credentials.h
    src/mainwindow.cpp
    src/mainwindow.h
    src/storage.cpp
    src/storage.h
)

qt_add_executable(minimal-qt ${APP_SOURCES})
target_link_libraries(minimal-qt PRIVATE Qt6::Widgets minimal-core qt6keychain)

set_target_properties(minimal-qt PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

if(ANDROID)
    set_property(TARGET minimal-qt PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/android")
    set_property(TARGET minimal-qt PROPERTY QT_ANDROID_APP_NAME "Minimal Qt")
    set_property(TARGET minimal-qt PROPERTY QT_ANDROID_VERSION_NAME "0.1")
    set_property(TARGET minimal-qt PROPERTY QT_ANDROID_VERSION_CODE 1)
    set_property(TARGET minimal-qt PROPERTY QT_ANDROID_MIN_SDK_VERSION 28)
    set_property(TARGET minimal-qt PROPERTY QT_ANDROID_TARGET_SDK_VERSION 34)
endif()

if(EMSCRIPTEN)
    # The core keeps a 24 MB arena as one static block, so the heap starts
    # above it rather than growing through it on the first vault operation.
    set_property(TARGET minimal-qt PROPERTY QT_WASM_INITIAL_MEMORY "128MB")
endif()

# Tests on the host only: the behaviour under test is the same code all three
# targets package.
if(NOT ANDROID AND NOT EMSCRIPTEN)
    find_package(Qt6 6.4 QUIET COMPONENTS Test)
    if(Qt6Test_FOUND)
        enable_testing()
        qt_add_executable(tst_minimal
            tests/tst_minimal.cpp
            src/clipboard.cpp
            src/clipboard.h
            src/core.cpp
            src/core.h
            src/credentials.cpp
            src/credentials.h
            src/mainwindow.cpp
            src/mainwindow.h
            src/storage.cpp
            src/storage.h
        )
        target_include_directories(tst_minimal PRIVATE src)
        target_link_libraries(tst_minimal PRIVATE Qt6::Widgets Qt6::Test minimal-core qt6keychain)
        add_test(NAME minimal COMMAND tst_minimal)
        set_tests_properties(minimal PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
