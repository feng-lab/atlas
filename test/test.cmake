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


# Atlas-aware test: links against the public Atlas library target, exporting
# all required include dirs and link dependencies (Qt, glbinding, etc.).
macro(add_atlas_gtest_executable test_name)
  add_executable(${test_name} ${CMAKE_CURRENT_LIST_DIR}/${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE GTest::gtest_main atlas_lib)
  target_compile_definitions(${test_name} PRIVATE ATLAS_TEST_DATA_DIR="${CMAKE_CURRENT_LIST_DIR}/../atlas_test_data")
  gtest_discover_tests(${test_name} DISCOVERY_TIMEOUT 600)
endmacro()

option(ATLAS_ENABLE_THIRD_PARTY_LIFETIME_TEST
       "Build vendored Fizz/Proxygen lifetime regression coverage"
       OFF)

# Targeted third-party regression coverage for vendored Fizz/Proxygen lifetime
# patches. This uses the shipped libraries plus selected vendored test headers
# instead of importing entire upstream test suites into the default Atlas build.
if (ATLAS_ENABLE_THIRD_PARTY_LIFETIME_TEST)
  find_package(fizz CONFIG REQUIRED
               PATHS ${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/build NO_DEFAULT_PATH)
  find_package(proxygen CONFIG REQUIRED
               PATHS ${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/build NO_DEFAULT_PATH)
  add_executable(zthirdpartylifetimetest
    ${CMAKE_CURRENT_LIST_DIR}/zthirdpartylifetimetest.cpp)
  target_include_directories(
    zthirdpartylifetimetest
    PRIVATE
      ${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/build/include)
  # Match the vendored Folly universal build's x86 feature level so the
  # generated object file references the same F14LinkCheck symbol variant.
  if (APPLE)
    target_compile_options(
      zthirdpartylifetimetest
      PRIVATE
        -mcpu=apple-m1
        -mavx)
  endif ()
  target_link_libraries(
    zthirdpartylifetimetest
    PRIVATE
      GTest::gtest_main
      GTest::gmock
      fizz::fizz
      proxygen::proxygen)
  gtest_discover_tests(zthirdpartylifetimetest DISCOVERY_TIMEOUT 600)
endif ()


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
add_gtest_executable(zimgometifftest)
add_gtest_executable(zimgtifftest)
add_gtest_executable(zimgregionalextrematest)
add_gtest_executable(zimgsigneddistancemaptest)
add_gtest_executable(zimgtest)
add_gtest_executable(zconcurrentlrucachetest)
add_executable(zbioformatstest ${CMAKE_CURRENT_LIST_DIR}/zbioformatstest.cpp ${CMAKE_CURRENT_LIST_DIR}/ztestmain.cpp)
target_link_libraries(zbioformatstest GTest::gtest img)
target_compile_definitions(zbioformatstest PRIVATE ATLAS_TEST_DATA_DIR="${CMAKE_CURRENT_LIST_DIR}/../atlas_test_data")
target_compile_definitions(zbioformatstest PRIVATE ATLAS_THIRDPARTY_BUILD_DIR="${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/build")
gtest_discover_tests(zbioformatstest DISCOVERY_TIMEOUT 600)
add_gtest_executable(zimgpngtest)
add_gtest_executable(zswceditlegacytest)
add_gtest_executable(zneutubetraceswclabelstacktest)
add_gtest_executable(zswcpostprocesstest)
add_gtest_executable(zblockedautotraceboundstest)
add_gtest_executable(zblockedautotracesessiontest)
add_gtest_executable(zswcspatialindextest)
add_gtest_executable(zneutubelocsegchaincircletest)
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
add_atlas_gtest_executable(zvulkanpipelinecontexttest)
add_atlas_gtest_executable(zimgometiffpacktest)

# Consolidate the heaviest Atlas-linked tests into a single executable to avoid paying
# the large atlas_lib link cost multiple times. This currently includes:
# - Block-ID collector generated-input regression coverage
# - Neuroglancer precomputed integration tests
# - Neuroglancer state/share-link parsing tests
# - ROI mask rasterization integration tests (historically `zroimaskrastertest`)
# - SQLite-backed HTTP disk cache tests (historically `zhttpdiskcachetest`)
add_gtest_executable(zneutubeswcsignalfittertest)
if (ATLAS_HAS_INTERNAL_NEUROLABI)
  add_gtest_executable(zneutubecommand2paritytest)
  target_link_libraries(zneutubecommand2paritytest neutu)
endif ()
# skip for now
# add_atlas_gtest_executable(zswcpackundomergetest)
add_executable(
  zatlasheavytest
  ${CMAKE_CURRENT_LIST_DIR}/z3dblockidcollectortest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zroimaskrastertest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zcameraparameteranimationtest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zcutspanparametertest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerprecomputedchunkdecodertest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerprecomputedannotationstest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerprecomputedsegmentpropertiestest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerprecomputedskeletontest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerprecomputede2etest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerstatetest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerurltest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zremoteobjectreadertest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zneuroglancershardedreadertest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zneuroglancerprecomputedmeshtest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zneuroglanceruint64shardingtest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zhttpretrypolicytest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zhttpsystemproxytest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zhttptruststoretest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zimgdiskcacheentrytest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zmarkdownbrowsertest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zflagfiledocumenttest.cpp
  ${CMAKE_CURRENT_LIST_DIR}/zhttpdiskcachetest.cpp)
target_link_libraries(zatlasheavytest PRIVATE GTest::gtest_main atlas_lib)
target_compile_definitions(zatlasheavytest PRIVATE ATLAS_TEST_DATA_DIR="${CMAKE_CURRENT_LIST_DIR}/../atlas_test_data")
gtest_discover_tests(zatlasheavytest DISCOVERY_TIMEOUT 600)


find_package(benchmark REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/build NO_DEFAULT_PATH)
print_target_properties(benchmark::benchmark)

add_executable(zbenchmark ${CMAKE_CURRENT_LIST_DIR}/zbenchmark.cpp)
target_link_libraries(zbenchmark atlas_lib benchmark::benchmark)
target_compile_definitions(zbenchmark PRIVATE ATLAS_THIRDPARTY_BUILD_DIR="${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/build")
