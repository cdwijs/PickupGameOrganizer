cmake_minimum_required(VERSION 3.21)
project(QtHello VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Qt 6 for desktop/Android/WebAssembly; Qt 5.15 for the MXE Windows builds,
# which only ship Qt 5.
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Widgets)
if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    qt_standard_project_setup()
endif()

add_subdirectory(src)

# Tests need a host-runnable binary; skip them when cross-compiling.
if(NOT CMAKE_CROSSCOMPILING AND NOT EMSCRIPTEN)
    enable_testing()
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
