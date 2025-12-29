cmake_minimum_required(VERSION 3.26)
project(zimg_python LANGUAGES CXX)

# Delegate to the existing CMake project that builds the nanobind module.
# This assumes you have third-party dependencies prepared as in the main repo build.

# Allow scikit-build-core to detect we are building for a wheel
set(SKBUILD ON)

# Use the repo's existing nanobind extension project under src/python.
# Allow an environment or cache override so CI can point at the full repo
# even when the Python build runs from an isolated copy (e.g., wheel build).
if(DEFINED ENV{ZIMG_SRC_DIR})
  set(ZIMG_SRC_DIR $ENV{ZIMG_SRC_DIR})
elseif(NOT DEFINED ZIMG_SRC_DIR)
  set(ZIMG_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/../../src/python)
endif()
if(NOT EXISTS "${ZIMG_SRC_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "Could not find src/python/CMakeLists.txt at ${ZIMG_SRC_DIR}")
endif()

add_subdirectory(${ZIMG_SRC_DIR} ${CMAKE_BINARY_DIR}/zimg_src)
