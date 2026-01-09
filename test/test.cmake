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

# Policy for heavy Atlas-linked tests:
# - Enabled on developer machines.
# - Disabled on Windows CI runners where link time/memory can be a bottleneck.
set(_atlas_is_windows_ci OFF)
if (WIN32 AND (DEFINED ENV{GITHUB_ACTIONS} OR DEFINED ENV{CI}))
  set(_atlas_is_windows_ci ON)
endif ()

# Vulkan RAII pipeline recorder debug checks (debug-only assertions in code)
# This test only exercises header + a few .cpp symbols; there is no GPU work.
# `zroimaskrastertest` is particularly slow/heavy on Windows runners. Skip it in
# CI to keep the default test build lightweight.
if (_atlas_is_windows_ci)
  message(STATUS "Skipping zroimaskrastertest on Windows CI (heavy link against atlas_lib).")
else ()
  add_atlas_gtest_executable(zroimaskrastertest)
endif ()
add_atlas_gtest_executable(zvulkanpipelinecontexttest)
add_atlas_gtest_executable(zneuroglanceruint64shardingtest)
add_atlas_gtest_executable(zneuroglancerprecomputedchunkdecodertest)
add_atlas_gtest_executable(zneuroglancerstatetest)
add_atlas_gtest_executable(zhttpdiskcachetest)

# Consolidate the heaviest Atlas-linked Neuroglancer tests into a single executable to
# avoid paying the large atlas_lib link cost multiple times.
# `zneuroglancerprecomputedtest` is a large executable that links against atlas_lib
# and is frequently too slow/heavy to build+link on Windows CI runners. Skip it in
# CI to keep the default test build lightweight.
if (_atlas_is_windows_ci)
  message(STATUS "Skipping zneuroglancerprecomputedtest on Windows CI (heavy link against atlas_lib).")
else ()
  add_executable(
    zneuroglancerprecomputedtest
    ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerprecomputedannotationstest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerprecomputedsegmentpropertiestest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerprecomputedskeletontest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerprecomputede2etest.cpp)
  target_link_libraries(zneuroglancerprecomputedtest PRIVATE GTest::gtest_main atlas_lib)
  target_compile_definitions(zneuroglancerprecomputedtest PRIVATE ATLAS_TEST_DATA_DIR="${CMAKE_CURRENT_LIST_DIR}/../atlas_test_data")
  if (UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_options(zneuroglancerprecomputedtest PRIVATE -fuse-ld=lld)
  endif ()
  gtest_discover_tests(zneuroglancerprecomputedtest DISCOVERY_TIMEOUT 600)
endif ()


find_package(benchmark REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/build NO_DEFAULT_PATH)
print_target_properties(benchmark::benchmark)

add_executable(zbenchmark ${CMAKE_CURRENT_LIST_DIR}/zbenchmark.cpp)
target_link_libraries(zbenchmark img benchmark::benchmark)
