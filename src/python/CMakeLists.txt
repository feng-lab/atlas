cmake_minimum_required(VERSION 3.19.3)
project(zimg)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15)
# set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# set_target_properties(MyTarget PROPERTIES CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)

list(INSERT CMAKE_PREFIX_PATH 0 ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build)
list(INSERT CMAKE_MODULE_PATH 0 ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build)
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

  #  # Fail if there are missing symbols, except when using Address Sanitizer
  #  # since libasan symbols are supposed to be preloaded.
  #  if (NOT ENABLE_ASAN)
  #    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,defs")
  #    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-z,defs")
  #  endif()

  set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")
endif ()

if (APPLE)
  set(CMAKE_INSTALL_RPATH "@loader_path/lib")
endif ()

if (UNIX)
  # Do not put non-global symbols in the output file's symbol table.
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-x")
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-x")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-x")
endif ()

LIST(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../cmake)

message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")

# generate a version string from git tag-hash and current build time
include(GetGitRevisionDescription)
git_describe(GIT_VERSION)
string(TIMESTAMP BUILD_DATE " build %Y-%m-%dT%H:%M:%S")
string(APPEND GIT_VERSION ${BUILD_DATE})
message(STATUS "GIT_VERSION: ${GIT_VERSION}")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../version/version.h.in ${CMAKE_CURRENT_SOURCE_DIR}/../version/version.h
               ESCAPE_QUOTES @ONLY)

include(BuildType)

if (NOT CMAKE_DEBUG_POSTFIX)
  set(CMAKE_DEBUG_POSTFIX d)
endif ()
message(STATUS "CMAKE_DEBUG_POSTFIX: " ${CMAKE_DEBUG_POSTFIX})

set(BUILD_WITH_CONDA ON)

if (DEFINED ENV{PREFIX})
  message(STATUS "PREFIX environment variable defined, conda build is running: " $ENV{PREFIX})
else ()
  set(ENV{PREFIX} $ENV{CONDA_EXE}/../..)
  message(STATUS "PREFIX environment variable not defined, set this for IDE: " $ENV{PREFIX})
endif ()

include(PrintCMakeTargetInfo)
include(LibsForImg)

# subprojects
add_subdirectory(../img condaimg)

# pybind11
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/pybind11 pybind11)
message(STATUS "PYTHON_VERSION_MAJOR: " ${PYTHON_VERSION_MAJOR})
message(STATUS "PYTHON_EXECUTABLE: " ${PYTHON_EXECUTABLE})

# build python extensions
if (PYTHON_VERSION_MAJOR)
  set(module_name _imgpy)
  pybind11_add_module(${module_name} imgpy.cpp)
  target_link_libraries(${module_name} PRIVATE condaimg)
  if (APPLE)
    target_compile_options(${module_name} PRIVATE -faligned-allocation)
    set(freeimagelibname libfreeimageplus.dylib)
    add_custom_command(TARGET ${module_name}
                       POST_BUILD
                       COMMAND install_name_tool -change
                       @rpath/libfreeimageplus.dylib
                       @loader_path/lib/libfreeimageplus.dylib
                       $<TARGET_FILE:${module_name}>
                       COMMAND ${CMAKE_COMMAND} -E copy ${FREEIMAGE_LIBRARIES}
                       $<TARGET_FILE_DIR:${module_name}>/${freeimagelibname}
                       COMMAND install_name_tool -id ${freeimagelibname}
                       $<TARGET_FILE_DIR:${module_name}>/${freeimagelibname}
                       )
  endif ()
  if (UNIX AND NOT APPLE)
    set(freeimagelibname libfreeimageplus.so.3)
    add_custom_command(TARGET ${module_name}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy ${FREEIMAGE_LIBRARIES}
                       $<TARGET_FILE_DIR:${module_name}>/${freeimagelibname}
                       )
  endif ()
endif ()

# target_include_directories(${module_name} SYSTEM BEFORE PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/include)

# install
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/" # source directory
        DESTINATION ${CMAKE_INSTALL_PREFIX}/zimg # target directory
        FILES_MATCHING PATTERN "*.py"
        PATTERN "*idea*" EXCLUDE
        PATTERN "*cmake*" EXCLUDE
        )
install(TARGETS ${module_name} DESTINATION ${CMAKE_INSTALL_PREFIX}/zimg)
if (WIN32)
  install(FILES ${FREEIMAGE_DLLS} DESTINATION ${CMAKE_INSTALL_PREFIX}/zimg)
else (WIN32)
  install(FILES $<TARGET_FILE_DIR:${module_name}>/${freeimagelibname} DESTINATION ${CMAKE_INSTALL_PREFIX}/zimg/lib)
  # this fix will not work if other python module loads the lower version of libstdc++.so first, so load zimg first
  if (UNIX AND NOT APPLE)
    file(GLOB gcc_stuff /lib/x86_64-linux-gnu/libstdc++.so* /lib/x86_64-linux-gnu/libgcc_s.so*)
    install(FILES ${gcc_stuff}
            DESTINATION ${CMAKE_INSTALL_PREFIX}/zimg/lib # target directory
            )
  endif ()
endif (WIN32)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/jars" # source directory
        DESTINATION ${CMAKE_INSTALL_PREFIX}/zimg # target directory
        FILES_MATCHING PATTERN "*.jar"
        PATTERN "*idea*" EXCLUDE
        PATTERN "*cmake*" EXCLUDE
        )
