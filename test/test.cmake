add_definitions(-DATLAS_WITH_TESTS)
set(TESTS
    ${CMAKE_CURRENT_LIST_DIR}/zrunbenchmark.cpp
    ${CMAKE_CURRENT_LIST_DIR}/zrunbenchmark.h
    ${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/googletest/src/gtest-all.cc
    ${CMAKE_CURRENT_LIST_DIR}/zunittest.h
    ${CMAKE_CURRENT_LIST_DIR}/zunittest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/zimggraphtest.h
    ${CMAKE_CURRENT_LIST_DIR}/zimagetoimagemetrictest.h
    ${CMAKE_CURRENT_LIST_DIR}/zimageutilstest.h
    ${CMAKE_CURRENT_LIST_DIR}/zimageaffinetransformtest.h
    ${CMAKE_CURRENT_LIST_DIR}/zimgncctest.h
    ${CMAKE_CURRENT_LIST_DIR}/zfilereadtest.h
    ${CMAKE_CURRENT_LIST_DIR}/zimgiteratortest.h
    ${CMAKE_CURRENT_LIST_DIR}/zsaturateoperationtest.h
    ${CMAKE_CURRENT_LIST_DIR}/zclustertest.h
    ${CMAKE_CURRENT_LIST_DIR}/zimgtest.h
    ${CMAKE_CURRENT_LIST_DIR}/zimgconnectedcomponentstest.h
    ${CMAKE_CURRENT_LIST_DIR}/zimgsigneddistancemaptest.h
    ${CMAKE_CURRENT_LIST_DIR}/zimgautothresholdtest.h
    ${CMAKE_CURRENT_LIST_DIR}/zimgregionalextrematest.h
    ${CMAKE_CURRENT_LIST_DIR}/ztreetest.h
    )
include_directories(${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/googletest/include)
include_directories(${BENCHMARK_INCLUDE_DIRS})

if (WIN32)
  set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/googletest/src/gtest-all.cc
                              PROPERTIES COMPILE_FLAGS /I${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/googletest)
else (WIN32)
  set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/googletest/src/gtest-all.cc
                              PROPERTIES COMPILE_FLAGS
                              "-I${CMAKE_CURRENT_LIST_DIR}/../src/3rdparty/googletest -Wno-deprecated")
  set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/zunittest.cpp PROPERTIES COMPILE_FLAGS "-Wno-deprecated")
endif (WIN32)
