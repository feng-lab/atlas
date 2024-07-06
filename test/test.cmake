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

add_executable(zclustertest ${CMAKE_CURRENT_LIST_DIR}/zclustertest.cpp)
target_link_libraries(zclustertest GTest::gtest_main img)
gtest_discover_tests(zclustertest DISCOVERY_TIMEOUT 600)

add_executable(zfilereadtest ${CMAKE_CURRENT_LIST_DIR}/zfilereadtest.cpp)
target_link_libraries(zfilereadtest GTest::gtest_main img)
gtest_discover_tests(zfilereadtest DISCOVERY_TIMEOUT 600)

add_executable(zimageaffinetransformtest ${CMAKE_CURRENT_LIST_DIR}/zimageaffinetransformtest.cpp)
target_link_libraries(zimageaffinetransformtest GTest::gtest_main img)
gtest_discover_tests(zimageaffinetransformtest DISCOVERY_TIMEOUT 600)

add_executable(zimagetoimagemetrictest ${CMAKE_CURRENT_LIST_DIR}/zimagetoimagemetrictest.cpp)
target_link_libraries(zimagetoimagemetrictest GTest::gtest_main img)
gtest_discover_tests(zimagetoimagemetrictest DISCOVERY_TIMEOUT 600)

add_executable(zimageutilstest ${CMAKE_CURRENT_LIST_DIR}/zimageutilstest.cpp)
target_link_libraries(zimageutilstest GTest::gtest_main img)
gtest_discover_tests(zimageutilstest DISCOVERY_TIMEOUT 600)

add_executable(zimgautothresholdtest ${CMAKE_CURRENT_LIST_DIR}/zimgautothresholdtest.cpp)
target_link_libraries(zimgautothresholdtest GTest::gtest_main img)
gtest_discover_tests(zimgautothresholdtest DISCOVERY_TIMEOUT 600)

add_executable(zimgconnectedcomponentstest ${CMAKE_CURRENT_LIST_DIR}/zimgconnectedcomponentstest.cpp)
target_link_libraries(zimgconnectedcomponentstest GTest::gtest_main img)
gtest_discover_tests(zimgconnectedcomponentstest DISCOVERY_TIMEOUT 600)

add_executable(zimggraphtest ${CMAKE_CURRENT_LIST_DIR}/zimggraphtest.cpp)
target_link_libraries(zimggraphtest GTest::gtest_main img)
gtest_discover_tests(zimggraphtest DISCOVERY_TIMEOUT 600)

add_executable(zimgiteratortest ${CMAKE_CURRENT_LIST_DIR}/zimgiteratortest.cpp)
target_link_libraries(zimgiteratortest GTest::gtest_main img)
gtest_discover_tests(zimgiteratortest DISCOVERY_TIMEOUT 600)

add_executable(zimgnccmkltest ${CMAKE_CURRENT_LIST_DIR}/zimgnccmkltest.cpp)
target_link_libraries(zimgnccmkltest GTest::gtest_main img)
gtest_discover_tests(zimgnccmkltest DISCOVERY_TIMEOUT 600)

add_executable(zimgnccpocketffttest ${CMAKE_CURRENT_LIST_DIR}/zimgnccpocketffttest.cpp)
target_link_libraries(zimgnccpocketffttest GTest::gtest_main img)
gtest_discover_tests(zimgnccpocketffttest DISCOVERY_TIMEOUT 600)

add_executable(zimgncctest ${CMAKE_CURRENT_LIST_DIR}/zimgncctest.cpp)
target_link_libraries(zimgncctest GTest::gtest_main img)
gtest_discover_tests(zimgncctest DISCOVERY_TIMEOUT 600)

add_executable(zimgregionalextrematest ${CMAKE_CURRENT_LIST_DIR}/zimgregionalextrematest.cpp)
target_link_libraries(zimgregionalextrematest GTest::gtest_main img)
gtest_discover_tests(zimgregionalextrematest DISCOVERY_TIMEOUT 600)

add_executable(zimgsigneddistancemaptest ${CMAKE_CURRENT_LIST_DIR}/zimgsigneddistancemaptest.cpp)
target_link_libraries(zimgsigneddistancemaptest GTest::gtest_main img)
gtest_discover_tests(zimgsigneddistancemaptest DISCOVERY_TIMEOUT 600)

add_executable(zimgtest ${CMAKE_CURRENT_LIST_DIR}/zimgtest.cpp)
target_link_libraries(zimgtest GTest::gtest_main img)
gtest_discover_tests(zimgtest DISCOVERY_TIMEOUT 600)

add_executable(zsaturateoperationtest ${CMAKE_CURRENT_LIST_DIR}/zsaturateoperationtest.cpp)
target_link_libraries(zsaturateoperationtest GTest::gtest_main img)
gtest_discover_tests(zsaturateoperationtest DISCOVERY_TIMEOUT 600)

add_executable(ztreetest ${CMAKE_CURRENT_LIST_DIR}/ztreetest.cpp)
target_link_libraries(ztreetest GTest::gtest_main img)
gtest_discover_tests(ztreetest DISCOVERY_TIMEOUT 600)

add_executable(zstatisticsutilstest ${CMAKE_CURRENT_LIST_DIR}/zstatisticsutilstest.cpp)
target_link_libraries(zstatisticsutilstest GTest::gtest_main img)
gtest_discover_tests(zstatisticsutilstest DISCOVERY_TIMEOUT 600)

add_executable(zstructutilstest ${CMAKE_CURRENT_LIST_DIR}/zstructutilstest.cpp)
target_link_libraries(zstructutilstest GTest::gtest_main img)
gtest_discover_tests(zstructutilstest DISCOVERY_TIMEOUT 600)

add_executable(zenumtest ${CMAKE_CURRENT_LIST_DIR}/zenumtest.cpp)
target_link_libraries(zenumtest GTest::gtest_main img)
gtest_discover_tests(zenumtest DISCOVERY_TIMEOUT 600)
