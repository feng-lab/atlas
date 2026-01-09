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


# Atlas-aware test: links against the Atlas static library, exporting
# all required include dirs and link dependencies (Qt, glbinding, etc.).
macro(add_atlas_gtest_executable test_name)
  add_executable(${test_name} ${CMAKE_CURRENT_LIST_DIR}/${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE GTest::gtest_main atlas_lib)
  target_compile_definitions(${test_name} PRIVATE ATLAS_TEST_DATA_DIR="${CMAKE_CURRENT_LIST_DIR}/../atlas_test_data")
  if (UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_options(${test_name} PRIVATE -fuse-ld=lld)
  endif ()
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

# Atlas-side tests

# Vulkan RAII pipeline recorder debug checks (debug-only assertions in code)
# This test only exercises header + a few .cpp symbols; there is no GPU work.
set(_atlas_enable_zroimaskrastertest_default ON)
if (WIN32 AND (DEFINED ENV{GITHUB_ACTIONS} OR DEFINED ENV{CI}))
  # `zroimaskrastertest` is particularly slow/heavy on Windows runners. Skip it in
  # CI by default, but keep it available for local Windows repro via
  # `-DATLAS_ENABLE_ZROIMASKRASTERTEST=ON`.
  set(_atlas_enable_zroimaskrastertest_default OFF)
endif ()
option(
  ATLAS_ENABLE_ZROIMASKRASTERTEST
  "Build and run zroimaskrastertest (slow on Windows CI)."
  ${_atlas_enable_zroimaskrastertest_default})
if (ATLAS_ENABLE_ZROIMASKRASTERTEST)
  add_atlas_gtest_executable(zroimaskrastertest)
else ()
  message(STATUS "Skipping zroimaskrastertest (ATLAS_ENABLE_ZROIMASKRASTERTEST=OFF)")
endif ()
add_atlas_gtest_executable(zvulkanpipelinecontexttest)
add_atlas_gtest_executable(zneuroglanceruint64shardingtest)
add_atlas_gtest_executable(zneuroglancerprecomputedchunkdecodertest)
add_atlas_gtest_executable(zneuroglancerstatetest)
add_atlas_gtest_executable(zneuroglancerprecomputedannotationstest)
add_atlas_gtest_executable(zhttpdiskcachetest)
add_atlas_gtest_executable(zneuroglancerprecomputedsegmentpropertiestest)
add_atlas_gtest_executable(zneuroglancerprecomputedskeletontest)
add_atlas_gtest_executable(zneuroglancerprecomputede2etest)


find_package(benchmark REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/build NO_DEFAULT_PATH)
print_target_properties(benchmark::benchmark)

add_executable(zbenchmark ${CMAKE_CURRENT_LIST_DIR}/zbenchmark.cpp)
target_link_libraries(zbenchmark img benchmark::benchmark)
