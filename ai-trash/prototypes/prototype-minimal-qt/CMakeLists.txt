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

set(APP_SOURCES
    src/main.cpp
    src/clipboard.cpp
    src/clipboard.h
    src/core.cpp
    src/core.h
    src/mainwindow.cpp
    src/mainwindow.h
    src/storage.cpp
    src/storage.h
)

qt_add_executable(minimal-qt ${APP_SOURCES})
target_link_libraries(minimal-qt PRIVATE Qt6::Widgets minimal-core)

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
            src/mainwindow.cpp
            src/mainwindow.h
            src/storage.cpp
            src/storage.h
        )
        target_include_directories(tst_minimal PRIVATE src)
        target_link_libraries(tst_minimal PRIVATE Qt6::Widgets Qt6::Test minimal-core)
        add_test(NAME minimal COMMAND tst_minimal)
        set_tests_properties(minimal PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
