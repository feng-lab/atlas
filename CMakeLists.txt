cmake_minimum_required(VERSION 3.6.1)
project(atlas)

LIST(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(GetGitRevisionDescription)
git_describe(GIT_VERSION)
string(TIMESTAMP BUILD_DATE " build %Y-%m-%dT%H:%M:%S")
string(APPEND GIT_VERSION ${BUILD_DATE})
message(${GIT_VERSION})
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/version/version.h.in ${CMAKE_CURRENT_SOURCE_DIR}/src/version/version.h
               ESCAPE_QUOTES @ONLY)
# set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../build-atlas)

add_subdirectory(src/img)
add_subdirectory(src/atlas)

