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
macro(add_atlas_core_gtest_executable test_name)
  add_executable(${test_name} ${CMAKE_CURRENT_LIST_DIR}/${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE GTest::gtest_main atlas_core)
  target_compile_definitions(${test_name} PRIVATE ATLAS_TEST_DATA_DIR="${CMAKE_CURRENT_LIST_DIR}/../atlas_test_data")
  if (UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_options(${test_name} PRIVATE -fuse-ld=lld)
  endif ()
  gtest_discover_tests(${test_name} DISCOVERY_TIMEOUT 600)
endmacro()

# Atlas-aware test: links against the Z3D codepath (OpenGL/shared rendering) plus core.
macro(add_atlas_z3d_gtest_executable test_name)
  add_executable(${test_name} ${CMAKE_CURRENT_LIST_DIR}/${test_name}.cpp)
  # Keep providers after dependents for linkers that care about order.
  target_link_libraries(${test_name} PRIVATE GTest::gtest_main atlas_z3d atlas_core)
  target_compile_definitions(${test_name} PRIVATE ATLAS_TEST_DATA_DIR="${CMAKE_CURRENT_LIST_DIR}/../atlas_test_data")
  if (UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_options(${test_name} PRIVATE -fuse-ld=lld)
  endif ()
  gtest_discover_tests(${test_name} DISCOVERY_TIMEOUT 600)
endmacro()

# Atlas-aware test: links against the Vulkan-only codepath plus core.
macro(add_atlas_vulkan_gtest_executable test_name)
  add_executable(${test_name} ${CMAKE_CURRENT_LIST_DIR}/${test_name}.cpp)
  # Keep providers after dependents for linkers that care about order.
  target_link_libraries(${test_name} PRIVATE GTest::gtest_main atlas_vulkan atlas_z3d atlas_core)
  target_compile_definitions(${test_name} PRIVATE ATLAS_TEST_DATA_DIR="${CMAKE_CURRENT_LIST_DIR}/../atlas_test_data")
  if (UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_options(${test_name} PRIVATE -fuse-ld=lld)
  endif ()
  gtest_discover_tests(${test_name} DISCOVERY_TIMEOUT 600)
endmacro()

# Full Atlas-linked test: links core + Z3D + Vulkan via the umbrella target.
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
add_gtest_executable(zswceditlegacytest)
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
if (NOT DEFINED _atlas_is_windows_ci)
  set(_atlas_is_windows_ci OFF)
  if (WIN32 AND (DEFINED ENV{GITHUB_ACTIONS} OR DEFINED ENV{CI}))
    set(_atlas_is_windows_ci ON)
  endif ()
endif ()

# Vulkan RAII pipeline recorder debug checks (debug-only assertions in code)
# This test only exercises header + a few .cpp symbols; there is no GPU work.
add_atlas_vulkan_gtest_executable(zvulkanpipelinecontexttest)
target_link_libraries(zvulkanpipelinecontexttest PRIVATE neutube)
add_atlas_core_gtest_executable(zneuroglanceruint64shardingtest)
add_atlas_core_gtest_executable(zneuroglancerstatetest)

# Consolidate the heaviest Atlas-linked tests into a single executable to avoid paying
# the large atlas_lib link cost multiple times. This currently includes:
# - Neuroglancer precomputed integration tests
# - ROI mask rasterization integration tests (historically `zroimaskrastertest`)
# - SQLite-backed HTTP disk cache tests (historically `zhttpdiskcachetest`)
# `zatlasheavytest` is a large executable that links against atlas_lib
# and is frequently too slow/heavy to build+link on Windows CI runners. Skip it in
# CI to keep the default test build lightweight.
if (_atlas_is_windows_ci)
  message(STATUS "Skipping zatlasheavytest on Windows CI (heavy link against atlas_lib).")
else ()
  add_gtest_executable(zneutubeswcsignalfittertest)
  target_link_libraries(zneutubeswcsignalfittertest neutube) 
  add_gtest_executable(zneutubecommand2paritytest)
  target_link_libraries(zneutubecommand2paritytest neutube neutu)
  add_atlas_z3d_gtest_executable(zswcpackundomergetest)
  target_link_libraries(zswcpackundomergetest PRIVATE neutube atlas_vulkan)
  add_executable(
    zatlasheavytest
    ${CMAKE_CURRENT_LIST_DIR}/zroimaskrastertest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/zcameraparameteranimationtest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/zcutspanparametertest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerprecomputedchunkdecodertest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerprecomputedannotationstest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerprecomputedsegmentpropertiestest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerprecomputedskeletontest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerprecomputede2etest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/zimgdiskcacheentrytest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/zhttpdiskcachetest.cpp)
  # This test suite does not depend on Vulkan. Avoid linking atlas_vulkan to keep
  # the link step lighter (especially on Windows CI and developer machines).
  target_link_libraries(zatlasheavytest PRIVATE GTest::gtest_main atlas_z3d atlas_core)
  target_compile_definitions(zatlasheavytest PRIVATE ATLAS_TEST_DATA_DIR="${CMAKE_CURRENT_LIST_DIR}/../atlas_test_data")
  if (UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_options(zatlasheavytest PRIVATE -fuse-ld=lld)
  endif ()
  gtest_discover_tests(zatlasheavytest DISCOVERY_TIMEOUT 600)
endif ()


find_package(benchmark REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/build NO_DEFAULT_PATH)
print_target_properties(benchmark::benchmark)

add_executable(zbenchmark ${CMAKE_CURRENT_LIST_DIR}/zbenchmark.cpp)
target_link_libraries(zbenchmark img benchmark::benchmark)
