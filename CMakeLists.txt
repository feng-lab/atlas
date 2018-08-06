cmake_minimum_required(VERSION 3.6.1)
project(atlas)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.10)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

LIST(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")

# generate a version string from git tag-hash and current build time
include(GetGitRevisionDescription)
git_describe(GIT_VERSION)
string(TIMESTAMP BUILD_DATE " build %Y-%m-%dT%H:%M:%S")
string(APPEND GIT_VERSION ${BUILD_DATE})
message(STATUS "GIT_VERSION: ${GIT_VERSION}")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/version/version.h.in ${CMAKE_CURRENT_SOURCE_DIR}/src/version/version.h
               ESCAPE_QUOTES @ONLY)
# set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../build-atlas)

include(BuildType)

if (NOT CMAKE_DEBUG_POSTFIX)
  set(CMAKE_DEBUG_POSTFIX d)
endif ()
message(STATUS "CMAKE_DEBUG_POSTFIX: " ${CMAKE_DEBUG_POSTFIX})

# qt
include(QtInfo)
message(STATUS "QT_VERSION: " ${QT_VERSION})
message(STATUS "QT_PATHS: " ${QT_PATHS})

# tbb
include(tbb/TBBMakeConfig)
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
  FIND_PATH(TBBROOT include/tbb/task_scheduler_init.h
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
  FIND_PATH(TBBROOT include/tbb/task_scheduler_init.h
            DOC "Root of TBB installation"
            HINTS ${TBBROOT}
            PATHS
            ${PROJECT_SOURCE_DIR}/tbb
            /opt/intel/composerxe/tbb
            /opt/intel/compilers_and_libraries/tbb
            /opt/intel/tbb
            )
endif (WIN32)
tbb_make_config(TBB_ROOT ${TBBROOT})
set(TBB_DIR ${TBBROOT}/cmake)

# subprojects
add_subdirectory(src/img)
add_subdirectory(src/atlas)

