add_library(QtHello_lib STATIC core.cpp core.h)
target_link_libraries(QtHello_lib PUBLIC Qt${QT_VERSION_MAJOR}::Core)
target_include_directories(QtHello_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    qt_add_executable(QtHello main.cpp)
else()
    add_executable(QtHello main.cpp)
endif()
target_link_libraries(QtHello PRIVATE QtHello_lib Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Widgets)

# WASM and Android both need the deployment metadata these generate.
set_target_properties(QtHello PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    qt_generate_deploy_app_script(
        TARGET QtHello
        OUTPUT_SCRIPT deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${deploy_script})
endif()
