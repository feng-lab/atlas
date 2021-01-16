cmake_minimum_required(VERSION 3.6.1)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.14)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (WIN32)
  set(CMAKE_SYSTEM_NAME Windows)
elseif (APPLE)
  set(CMAKE_SYSTEM_NAME Darwin)
else ()
  set(CMAKE_SYSTEM_NAME Linux)
endif ()

# tbb
include(${CMAKE_CURRENT_LIST_DIR}/tbb/TBBMakeConfig.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/PathList.cmake)
if (WIN32)
  FIND_PATH(TBBROOT include/tbb/tbb.h
            DOC "Root of TBB installation"
            HINTS ${TBBROOT}
            PATHS
            ${INTEL_PATH}/tbb
            )
else (WIN32)
  FIND_PATH(TBBROOT include/tbb/tbb.h
            DOC "Root of TBB installation"
            HINTS ${TBBROOT}
            PATHS
            ${INTEL_PATH}/tbb
            )
endif (WIN32)
message(STATUS "TBBROOT: ${TBBROOT}")
tbb_make_config(TBB_ROOT ${TBBROOT} SAVE_TO ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build)
