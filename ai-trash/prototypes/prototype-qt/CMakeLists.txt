# One target, three platforms. Nothing here is per-platform except the Android
# packaging block: qt_add_executable produces an ELF binary on Linux, a
# .wasm + .js + .html bundle for WebAssembly, and a shared library that
# androiddeployqt wraps into an APK for Android.
cmake_minimum_required(VERSION 3.21)

project(prototype-qt VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)

# Turns on AUTOMOC/AUTOUIC/AUTORCC and the Qt install layout.
qt_standard_project_setup()

qt_add_executable(counter src/main.cpp src/counterwindow.h)

target_link_libraries(counter PRIVATE Qt6::Widgets)

set_target_properties(counter PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

if(ANDROID)
    set_property(TARGET counter PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/android")
    set_property(TARGET counter PROPERTY QT_ANDROID_APP_NAME "Qt counter")
    set_property(TARGET counter PROPERTY QT_ANDROID_VERSION_NAME "0.1")
    set_property(TARGET counter PROPERTY QT_ANDROID_VERSION_CODE 1)
    set_property(TARGET counter PROPERTY QT_ANDROID_MIN_SDK_VERSION 28)
    set_property(TARGET counter PROPERTY QT_ANDROID_TARGET_SDK_VERSION 34)
endif()

if(EMSCRIPTEN)
    # The default 16 MB heap is enough for a widget or two, but the growth step
    # is a full copy of linear memory; starting higher avoids it on load.
    set_property(TARGET counter PROPERTY QT_WASM_INITIAL_MEMORY "128MB")
endif()

# Tests, on the host only: the point of them is the widget's behaviour, which is
# the same code on all three targets, and the offscreen platform runs it without
# a display server. Qt6::Test is not in every Qt build, so a missing one skips
# them rather than failing the configure step.
if(NOT ANDROID AND NOT EMSCRIPTEN)
    find_package(Qt6 6.4 QUIET COMPONENTS Test)
    if(Qt6Test_FOUND)
        enable_testing()
        qt_add_executable(tst_counter tests/tst_counter.cpp src/counterwindow.h)
        target_include_directories(tst_counter PRIVATE src)
        target_link_libraries(tst_counter PRIVATE Qt6::Widgets Qt6::Test)
        add_test(NAME counter COMMAND tst_counter)
        set_tests_properties(counter PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
