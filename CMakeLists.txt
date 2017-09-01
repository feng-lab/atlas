cmake_minimum_required(VERSION 3.6.1)
project(atlas)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.9)

LIST(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

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
set(QT_VERSION 5.9.1)
set(QT_PATHS ~/Qt/${QT_VERSION}/clang_64 ~/Qt/${QT_VERSION}/gcc_64 C:/Qt/Qt/${QT_VERSION}/msvc2017_64)
message(STATUS "QT_VERSION: " ${QT_VERSION})
message(STATUS "QT_PATHS: " ${QT_PATHS})

# subprojects
add_subdirectory(src/img)
add_subdirectory(src/atlas)

