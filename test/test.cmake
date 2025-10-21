include(CTest)

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

message(STATUS "ATLAS_TEST_DATA_DIR is set to ${CMAKE_CURRENT_LIST_DIR}/../atlas_test_data")

macro(add_gtest_executable test_name)
  add_executable(${test_name} ${CMAKE_CURRENT_LIST_DIR}/${test_name}.cpp)
  target_link_libraries(${test_name} GTest::gtest_main img)
  target_compile_definitions(${test_name} PRIVATE ATLAS_TEST_DATA_DIR="${CMAKE_CURRENT_LIST_DIR}/../atlas_test_data")
  gtest_discover_tests(${test_name} DISCOVERY_TIMEOUT 600)
endmacro()


add_gtest_executable(zclustertest)
add_gtest_executable(zfilereadtest)
add_gtest_executable(zimageaffinetransformtest)
add_gtest_executable(zimagetoimagemetrictest)
add_gtest_executable(zimageutilstest)
add_gtest_executable(zimgautothresholdtest)
add_gtest_executable(zimgconnectedcomponentstest)
add_gtest_executable(zimggraphtest)
add_gtest_executable(zimgiteratortest)
add_gtest_executable(zimgnccmkltest)
add_gtest_executable(zimgnccpocketffttest)
add_gtest_executable(zimgncctest)
add_gtest_executable(zimgregionalextrematest)
add_gtest_executable(zimgsigneddistancemaptest)
add_gtest_executable(zimgtest)
add_gtest_executable(zsaturateoperationtest)
add_gtest_executable(ztreetest)
add_gtest_executable(zstatisticsutilstest)
add_gtest_executable(zstructutilstest)
add_gtest_executable(zenumtest)
add_gtest_executable(zstringutilstest)
add_gtest_executable(ztupleliketest)
# add_gtest_executable(zvulkanpipelinecontexttest)

# Atlas-side tests

find_package(benchmark REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/build NO_DEFAULT_PATH)
print_target_properties(benchmark::benchmark)

add_executable(zbenchmark ${CMAKE_CURRENT_LIST_DIR}/zbenchmark.cpp)
target_link_libraries(zbenchmark img benchmark::benchmark)
