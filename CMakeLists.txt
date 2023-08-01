cmake_minimum_required(VERSION 3.17.5)
project(atlas)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0)
# set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# set_target_properties(MyTarget PROPERTIES CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)

list(INSERT CMAKE_PREFIX_PATH 0 ${CMAKE_CURRENT_SOURCE_DIR}/src/3rdparty/build)
list(INSERT CMAKE_MODULE_PATH 0 ${CMAKE_CURRENT_SOURCE_DIR}/src/3rdparty/build)
set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a ${CMAKE_FIND_LIBRARY_SUFFIXES})

if (UNIX AND NOT APPLE)
  # excludes symbols in all archive libraries from automatic export.
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--exclude-libs,ALL")
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,--exclude-libs,ALL")
  # Prevent symbol relocations internal to our wrapper library to be
  # overwritten by the application.  And fail if there are missing
  # symbols.
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-Bsymbolic -Wl,-Bsymbolic-functions")
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-Bsymbolic -Wl,-Bsymbolic-functions")

  # Fail if there are missing symbols, except when using Address Sanitizer
  # since libasan symbols are supposed to be preloaded.
  if (NOT ENABLE_ASAN)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,defs")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-z,defs")
  endif ()
endif ()

if (UNIX)
  # Do not put non-global symbols in the output file's symbol table.
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-x")
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-x")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-x")
endif ()

LIST(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src/cmake)

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

option(ATLAS_SANITIZE_ADDRESS "Build Atlas with Address Sanitizer enabled and debug symbols." OFF)
if (ATLAS_SANITIZE_ADDRESS)
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g")
  add_compile_definitions(ATLAS_SANITIZE_ADDRESS)
endif ()
message(STATUS "ATLAS_SANITIZE_ADDRESS: ${ATLAS_SANITIZE_ADDRESS}")

option(ATLAS_DEBUG_VERSION "Build Atlas with debug symbols and O1." OFF)
if (ATLAS_DEBUG_VERSION)
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g -O1")
  add_compile_definitions(ATLAS_DEBUG_VERSION)
endif ()
message(STATUS "ATLAS_DEBUG_VERSION: ${ATLAS_DEBUG_VERSION}")

include(PrintCMakeTargetInfo)


# subprojects
add_subdirectory(src/neurolabi/c)

include(LibsForImg)
add_subdirectory(src/neurolabi/gui)
add_subdirectory(src/img)
# add_subdirectory(src/matlab)
add_subdirectory(src/atlas)

option(BUILD_TESTING "Enable tests" ON)
if (BUILD_TESTING AND (NOT ATLAS_SANITIZE_ADDRESS) AND (NOT ATLAS_DEBUG_VERSION) AND (NOT CMAKE_CROSSCOMPILING))
  include(test/test.cmake)
endif ()
