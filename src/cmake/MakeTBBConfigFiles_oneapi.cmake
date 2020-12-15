cmake_minimum_required(VERSION 3.6.1)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.10)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# tbb
include(${CMAKE_CURRENT_LIST_DIR}/tbb/config_generation_my.cmake)
if (WIN32)
  # workaround for parentheses in variable name / CMP0053
  SET(PROGRAMFILESx86 "PROGRAMFILES(x86)")
  SET(PROGRAMFILES32 "$ENV{${PROGRAMFILESx86}}")
  IF (NOT PROGRAMFILES32)
    SET(PROGRAMFILES32 "$ENV{PROGRAMFILES}")
  ENDIF ()
  IF (NOT PROGRAMFILES32)
    SET(PROGRAMFILES32 "C:/Program Files (x86)")
  ENDIF ()
  FIND_PATH(TBBROOT include/tbb/tbb.h
            DOC "Root of TBB installation"
            HINTS ${TBBROOT}
            PATHS
            ${PROJECT_SOURCE_DIR}/tbb
            ${PROJECT_SOURCE_DIR}/../tbb
            "${PROGRAMFILES32}/IntelSWTools/compilers_and_libraries/windows/tbb"
            "${PROGRAMFILES32}/Intel/Composer XE/tbb"
            "${PROGRAMFILES32}/Intel/compilers_and_libraries/windows/tbb"
            )
else (WIN32)
  FIND_PATH(TBBROOT include/oneapi/tbb.h
            DOC "Root of TBB installation"
            HINTS ${TBBROOT}
            PATHS
            ${PROJECT_SOURCE_DIR}/tbb
            /opt/intel/oneapi/tbb/latest
            )
endif (WIN32)
message(STATUS "TBBROOT: ${TBBROOT}")

set(INC_REL_PATH "${TBBROOT}/include")
set(LIB_REL_PATH "${TBBROOT}/lib")
set(DLL_REL_PATH "${TBBROOT}/redist")

# Parse version info
file(READ ${INC_REL_PATH}/oneapi/tbb/version.h _tbb_version_info)
string(REGEX REPLACE ".*#define TBB_VERSION_MAJOR ([0-9]+).*" "\\1" _tbb_ver_major "${_tbb_version_info}")
string(REGEX REPLACE ".*#define TBB_VERSION_MINOR ([0-9]+).*" "\\1" _tbb_ver_minor "${_tbb_version_info}")
string(REGEX REPLACE ".*#define TBB_INTERFACE_VERSION ([0-9]+).*" "\\1" _tbb_interface_ver "${_tbb_version_info}")
string(REGEX REPLACE ".*#define __TBB_BINARY_VERSION ([0-9]+).*" "\\1" TBB_BINARY_VERSION "${_tbb_version_info}")

set(TBBMALLOC_BINARY_VERSION 2)
set(TBBBIND_BINARY_VERSION 3)
# file(READ ${CMAKE_CURRENT_LIST_DIR}/../../CMakeLists.txt _tbb_cmakelist)
# string(REGEX REPLACE ".*TBBMALLOC_BINARY_VERSION ([0-9]+).*" "\\1" TBBMALLOC_BINARY_VERSION "${_tbb_cmakelist}")
set(TBBMALLOC_PROXY_BINARY_VERSION ${TBBMALLOC_BINARY_VERSION})
# string(REGEX REPLACE ".*TBBBIND_BINARY_VERSION ([0-9]+).*" "\\1" TBBBIND_BINARY_VERSION "${_tbb_cmakelist}")

# Parse patch and tweak versions from interface version: e.g. 12014 --> 01 - patch version, 4 - tweak version.
math(EXPR _tbb_ver_patch "${_tbb_interface_ver} % 1000 / 10")
math(EXPR _tbb_ver_tweak "${_tbb_interface_ver} % 10")

set(COMMON_ARGS
    LIB_REL_PATH ${LIB_REL_PATH}
    VERSION ${_tbb_ver_major}.${_tbb_ver_minor}.${_tbb_ver_patch}.${_tbb_ver_tweak}
    TBB_BINARY_VERSION ${TBB_BINARY_VERSION}
    TBBMALLOC_BINARY_VERSION ${TBBMALLOC_BINARY_VERSION}
    TBBMALLOC_PROXY_BINARY_VERSION ${TBBMALLOC_PROXY_BINARY_VERSION}
    TBBBIND_BINARY_VERSION ${TBBBIND_BINARY_VERSION}
)

set(INSTALL_DIR ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build)
if (WIN32)
tbb_generate_config(INSTALL_DIR ${INSTALL_DIR} SYSTEM_NAME Windows INC_REL_PATH ${INC_REL_PATH} HANDLE_SUBDIRS DLL_REL_PATH ${DLL_REL_PATH} ${COMMON_ARGS})
elseif (APPLE)
tbb_generate_config(INSTALL_DIR ${INSTALL_DIR} SYSTEM_NAME Darwin  INC_REL_PATH ${INC_REL_PATH} ${COMMON_ARGS})
else ()
tbb_generate_config(INSTALL_DIR ${INSTALL_DIR} SYSTEM_NAME Linux   INC_REL_PATH ${INC_REL_PATH} HANDLE_SUBDIRS ${COMMON_ARGS})
endif ()
message(STATUS "TBBConfig files were created in ${INSTALL_DIR}")
