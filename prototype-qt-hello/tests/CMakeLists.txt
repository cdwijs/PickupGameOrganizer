add_executable(tst_core tst_core.cpp)
target_link_libraries(tst_core PRIVATE QtHello_lib Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME tst_core COMMAND tst_core)
