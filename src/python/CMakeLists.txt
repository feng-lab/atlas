cmake_minimum_required(VERSION 3.19.3)
project(zimg)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0)
# set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# set_target_properties(MyTarget PROPERTIES CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)

if (UNIX)
  list(APPEND CMAKE_IGNORE_PREFIX_PATH /usr/local)
  list(APPEND CMAKE_IGNORE_PATH /usr/local/bin)
endif ()
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

# nanobind
# Try to locate the Python interpreter and development headers before nanobind
find_package(Python 3.12 REQUIRED COMPONENTS Interpreter Development.Module OPTIONAL_COMPONENTS Development.SABIModule)

# Fallback: on some CMake/Python combinations, Python_INCLUDE_DIRS is only
# partially populated when requesting Development.Module. Pull from the target.
if (NOT Python_INCLUDE_DIRS AND TARGET Python::Module)
  get_target_property(_PY_INC Python::Module INTERFACE_INCLUDE_DIRECTORIES)
  if (_PY_INC)
    set(Python_INCLUDE_DIRS ${_PY_INC})
  endif ()
endif ()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/nanobind nanobind)

# build python extensions
set(module_name _imgpy)

# ABI toggles: regular (default), stable ABI, and free-threaded
option(IMG_PY_STABLE_ABI "Build the Python module with CPython's Stable ABI (requires CPython>=3.12)" ON)
option(IMG_PY_FREE_THREADED "Build free-threaded (nogil) module; requires a free-threaded Python interpreter" OFF)

set(NB_ARGS NOMINSIZE)
if (IMG_PY_STABLE_ABI AND IMG_PY_FREE_THREADED)
  message(WARNING "IMG_PY_STABLE_ABI and IMG_PY_FREE_THREADED are mutually exclusive. The stable ABI will be disabled for free-threaded builds.")
endif ()
if (IMG_PY_STABLE_ABI)
  list(APPEND NB_ARGS STABLE_ABI)
endif ()
if (IMG_PY_FREE_THREADED)
  list(APPEND NB_ARGS FREE_THREADED)
endif ()

nanobind_add_module(${module_name} ${NB_ARGS} imgpy.cpp)
target_link_libraries(${module_name} PRIVATE condaimg)

if (APPLE)
  target_compile_options(${module_name} PRIVATE -faligned-allocation)
  set(freeimagelibname libfreeimageplus.dylib)
  add_custom_command(TARGET ${module_name}
                     POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy ${FREEIMAGE_LIBRARIES}
                     $<TARGET_FILE_DIR:${module_name}>/${freeimagelibname}
  )
  set(tbblibname libtbb.12.dylib)
  add_custom_command(TARGET ${module_name}
                     POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:TBB::tbb>
                     $<TARGET_FILE_DIR:${module_name}>/${tbblibname}
  )
endif ()

# target_include_directories(${module_name} SYSTEM BEFORE PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/include)

# install
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/" # source directory
        DESTINATION zimg # target directory (relative so --prefix/DESTDIR work)
        FILES_MATCHING PATTERN "*.py"
        PATTERN "*idea*" EXCLUDE
        PATTERN "*cmake*" EXCLUDE
)
install(TARGETS ${module_name} DESTINATION zimg)

if (WIN32)
  #  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/bin/h5ls.exe DESTINATION ${CMAKE_INSTALL_PREFIX}/zimg)
  install(FILES ${FREEIMAGE_DLLS} DESTINATION zimg)
  install(FILES $<TARGET_FILE:TBB::tbb> DESTINATION zimg)
  install(FILES $<TARGET_FILE:Qt6::Core> DESTINATION zimg)
  install(FILES $<TARGET_FILE:Qt6::Gui> DESTINATION zimg)
endif (WIN32)

if (APPLE)
  install(FILES $<TARGET_FILE_DIR:${module_name}>/${freeimagelibname} DESTINATION zimg/lib)
  install(FILES $<TARGET_FILE_DIR:${module_name}>/${tbblibname} DESTINATION zimg/lib)
  install(FILES $<TARGET_FILE:Qt6::Core> DESTINATION zimg/lib/QtCore.framework/Versions/A)
  install(FILES $<TARGET_FILE:Qt6::Gui> DESTINATION zimg/lib/QtGui.framework/Versions/A)
  install(FILES $<TARGET_FILE:Qt6::DBus> DESTINATION zimg/lib/QtDBus.framework/Versions/A)

  option(IMG_PY_CODESIGN_INSTALLED_BINARIES "Codesign installed Mach-O binaries on macOS (ensures PyPI wheel imports cleanly)" ON)
  if (IMG_PY_CODESIGN_INSTALLED_BINARIES)
    # The Qt framework binaries we ship in the wheel are not full framework bundles
    # (we avoid symlinks / bundle resources). Their original upstream signatures
    # fail `codesign --verify` in this layout. Re-signing the installed Mach-O
    # files fixes runtime loading in downstream environments.
    #
    # Signing happens at install time so the wheel RECORD hashes are computed on
    # the signed artifacts.
    install(CODE [[
      set(_install_root "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")

      set(_codesign_identity "$ENV{MACOS_CODESIGN_IDENTITY}")
      if(_codesign_identity STREQUAL "")
        set(_codesign_identity "-")
      endif()

      set(_sign_targets
        "${_install_root}/zimg/_imgpy.abi3.so"
        "${_install_root}/zimg/lib/libfreeimageplus.dylib"
        "${_install_root}/zimg/lib/libtbb.12.dylib"
        "${_install_root}/zimg/lib/QtCore.framework/Versions/A/QtCore"
        "${_install_root}/zimg/lib/QtGui.framework/Versions/A/QtGui"
        "${_install_root}/zimg/lib/QtDBus.framework/Versions/A/QtDBus"
      )

      foreach(_path IN LISTS _sign_targets)
        if(NOT EXISTS "${_path}")
          message(STATUS "codesign: skipping missing file: ${_path}")
          continue()
        endif()

        execute_process(
          COMMAND codesign --force --sign "${_codesign_identity}" --timestamp=none "${_path}"
          RESULT_VARIABLE _cs_rc
          OUTPUT_VARIABLE _cs_out
          ERROR_VARIABLE _cs_err
        )
        if(NOT _cs_rc EQUAL 0)
          message(FATAL_ERROR "codesign failed for ${_path} (exit ${_cs_rc})\n${_cs_out}\n${_cs_err}")
        endif()

        execute_process(
          COMMAND codesign --verify --strict --verbose=2 "${_path}"
          RESULT_VARIABLE _verify_rc
          OUTPUT_VARIABLE _verify_out
          ERROR_VARIABLE _verify_err
        )
        if(NOT _verify_rc EQUAL 0)
          message(FATAL_ERROR "codesign verify failed for ${_path} (exit ${_verify_rc})\n${_verify_out}\n${_verify_err}")
        endif()
      endforeach()
    ]])
  endif()
endif ()

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/jars" # source directory
        DESTINATION zimg # target directory (relative so --prefix/DESTDIR work)
        FILES_MATCHING PATTERN "*.jar"
        PATTERN "*idea*" EXCLUDE
        PATTERN "*cmake*" EXCLUDE
)
