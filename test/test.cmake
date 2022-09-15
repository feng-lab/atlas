include(FetchContent)
FetchContent_Declare(
  googletest
  SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/googletest
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

add_executable(zclustertest ${CMAKE_CURRENT_LIST_DIR}/zclustertest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(zclustertest GTest::gtest_main img)
gtest_discover_tests(zclustertest)

add_executable(zfilereadtest ${CMAKE_CURRENT_LIST_DIR}/zfilereadtest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(zfilereadtest GTest::gtest_main img)
gtest_discover_tests(zfilereadtest)

add_executable(zimageaffinetransformtest ${CMAKE_CURRENT_LIST_DIR}/zimageaffinetransformtest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(zimageaffinetransformtest GTest::gtest_main img)
gtest_discover_tests(zimageaffinetransformtest)

add_executable(zimagetoimagemetrictest ${CMAKE_CURRENT_LIST_DIR}/zimagetoimagemetrictest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(zimagetoimagemetrictest GTest::gtest_main img)
gtest_discover_tests(zimagetoimagemetrictest)

add_executable(zimageutilstest ${CMAKE_CURRENT_LIST_DIR}/zimageutilstest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(zimageutilstest GTest::gtest_main img)
gtest_discover_tests(zimageutilstest)

add_executable(zimgautothresholdtest ${CMAKE_CURRENT_LIST_DIR}/zimgautothresholdtest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(zimgautothresholdtest GTest::gtest_main img)
gtest_discover_tests(zimgautothresholdtest)

add_executable(zimgconnectedcomponentstest ${CMAKE_CURRENT_LIST_DIR}/zimgconnectedcomponentstest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(zimgconnectedcomponentstest GTest::gtest_main img)
gtest_discover_tests(zimgconnectedcomponentstest)

add_executable(zimggraphtest ${CMAKE_CURRENT_LIST_DIR}/zimggraphtest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(zimggraphtest GTest::gtest_main img)
gtest_discover_tests(zimggraphtest)

add_executable(zimgiteratortest ${CMAKE_CURRENT_LIST_DIR}/zimgiteratortest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(zimgiteratortest GTest::gtest_main img)
gtest_discover_tests(zimgiteratortest)

add_executable(zimgnccmkltest ${CMAKE_CURRENT_LIST_DIR}/zimgnccmkltest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(zimgnccmkltest GTest::gtest_main img)
gtest_discover_tests(zimgnccmkltest)

add_executable(zimgnccpocketffttest ${CMAKE_CURRENT_LIST_DIR}/zimgnccpocketffttest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(zimgnccpocketffttest GTest::gtest_main img)
gtest_discover_tests(zimgnccpocketffttest)

add_executable(zimgncctest ${CMAKE_CURRENT_LIST_DIR}/zimgncctest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(zimgncctest GTest::gtest_main img)
gtest_discover_tests(zimgncctest)

add_executable(zimgregionalextrematest ${CMAKE_CURRENT_LIST_DIR}/zimgregionalextrematest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(zimgregionalextrematest GTest::gtest_main img)
gtest_discover_tests(zimgregionalextrematest)

add_executable(zimgsigneddistancemaptest ${CMAKE_CURRENT_LIST_DIR}/zimgsigneddistancemaptest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(zimgsigneddistancemaptest GTest::gtest_main img)
gtest_discover_tests(zimgsigneddistancemaptest)

add_executable(zimgtest ${CMAKE_CURRENT_LIST_DIR}/zimgtest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(zimgtest GTest::gtest_main img)
gtest_discover_tests(zimgtest)

add_executable(zsaturateoperationtest ${CMAKE_CURRENT_LIST_DIR}/zsaturateoperationtest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(zsaturateoperationtest GTest::gtest_main img)
gtest_discover_tests(zsaturateoperationtest)

add_executable(ztreetest ${CMAKE_CURRENT_LIST_DIR}/ztreetest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztest.cpp)
target_link_libraries(ztreetest GTest::gtest_main img)
gtest_discover_tests(ztreetest)
