# Python stable-ABI builds (Development.SABIModule / Python::SABIModule) require
# CMake >= 3.26 (FindPython).
cmake_minimum_required(VERSION 3.26)
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

include(PrintCMakeTargetInfo)

option(ZIMG_DISABLE_FREEIMAGE
       "Disable FreeImage support in img (removes FreeImage formats and avoids linking libfreeimageplus)."
       OFF)
include(LibsForImg)

# subprojects
if (NOT TARGET img)
  add_subdirectory(../img zimg_img)
endif ()

# ABI toggles: regular (default), stable ABI, and free-threaded
option(IMG_PY_STABLE_ABI "Build the Python module with CPython's Stable ABI (requires CPython>=3.12)" ON)
option(IMG_PY_FREE_THREADED "Build free-threaded (nogil) module; requires a free-threaded Python interpreter" OFF)

if (IMG_PY_STABLE_ABI AND IMG_PY_FREE_THREADED)
  message(WARNING "IMG_PY_STABLE_ABI and IMG_PY_FREE_THREADED are mutually exclusive. Disabling IMG_PY_STABLE_ABI for free-threaded builds.")
  set(IMG_PY_STABLE_ABI OFF)
endif ()

# Packaging: strip debug symbols from installed binaries to reduce wheel/conda size.
set(_img_py_strip_default OFF)
if (UNIX AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_img_py_strip_default ON)
endif ()
option(IMG_PY_STRIP_INSTALLED_BINARIES
       "Strip debug symbols from installed native binaries (reduces wheel size)."
       ${_img_py_strip_default})

set(_IMG_PY_STRIP_TOOL "")
if (IMG_PY_STRIP_INSTALLED_BINARIES)
  if (NOT UNIX)
    message(FATAL_ERROR "IMG_PY_STRIP_INSTALLED_BINARIES is only supported on UNIX-like platforms.")
  endif ()
  if (CMAKE_STRIP)
    set(_IMG_PY_STRIP_TOOL "${CMAKE_STRIP}")
  else ()
    find_program(_IMG_PY_STRIP_TOOL NAMES llvm-strip strip)
  endif ()
  if (NOT _IMG_PY_STRIP_TOOL)
    message(FATAL_ERROR "IMG_PY_STRIP_INSTALLED_BINARIES is enabled, but no strip tool was found.")
  endif ()
endif ()

# nanobind
# Locate the Python interpreter and development headers before including nanobind.
# Note: the requested version is a minimum; building against Python >= 3.12 is OK.
set(_img_py_python_components Interpreter Development.Module)
if (IMG_PY_STABLE_ABI)
  list(APPEND _img_py_python_components Development.SABIModule)
endif ()
find_package(Python 3.12 REQUIRED COMPONENTS ${_img_py_python_components})
unset(_img_py_python_components)

if (IMG_PY_STABLE_ABI)
  # nanobind's STABLE_ABI mode uses Py_LIMITED_API=0x030C0000 (cp312).
  # Ensure the selected toolchain can actually build an abi3 module; do not
  # silently fall back to a version-specific extension.
  if (NOT Python_INTERPRETER_ID STREQUAL "Python")
    message(FATAL_ERROR "IMG_PY_STABLE_ABI requires CPython, but FindPython detected '${Python_INTERPRETER_ID}' (Python_EXECUTABLE='${Python_EXECUTABLE}').")
  endif ()
  if (NOT TARGET Python::SABIModule)
    message(FATAL_ERROR
            "IMG_PY_STABLE_ABI is enabled, but CMake could not provide Python::SABIModule.\n"
            "This usually means the selected Python installation is missing development headers/libraries, or CMake is too old to support Development.SABIModule.\n"
            "Fixes:\n"
            "  - Install the Python development package/headers for your interpreter\n"
            "  - Configure with -DPython_EXECUTABLE=/path/to/python3.14 (or 3.15) to pick the intended interpreter\n"
            "  - Ensure you are using CMake >= 3.26")
  endif ()
  if (DEFINED Python_SOABI AND NOT Python_SOABI STREQUAL "" AND Python_SOABI MATCHES "^[^-]*-[^-]*t-")
    message(FATAL_ERROR "IMG_PY_STABLE_ABI does not support free-threaded CPython interpreters (SOABI='${Python_SOABI}'). Use a non-free-threaded CPython build or set -DIMG_PY_STABLE_ABI=OFF.")
  endif ()
endif ()

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

set(NB_ARGS NOMINSIZE)
if (IMG_PY_STABLE_ABI)
  list(APPEND NB_ARGS STABLE_ABI)
endif ()
if (IMG_PY_FREE_THREADED)
  list(APPEND NB_ARGS FREE_THREADED)
endif ()

nanobind_add_module(${module_name} ${NB_ARGS} imgpy.cpp)
target_link_libraries(${module_name} PRIVATE img)

if (APPLE)
  target_compile_options(${module_name} PRIVATE -faligned-allocation)
  set(freeimagelibname libfreeimageplus.dylib)
  if (NOT ZIMG_DISABLE_FREEIMAGE)
    add_custom_command(TARGET ${module_name}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy ${FREEIMAGE_LIBRARIES}
                       $<TARGET_FILE_DIR:${module_name}>/${freeimagelibname}
    )
  endif ()
  set(tbblibname libtbb.12.dylib)
  add_custom_command(TARGET ${module_name}
                     POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:TBB::tbb>
                     $<TARGET_FILE_DIR:${module_name}>/${tbblibname}
  )
endif ()

if (UNIX AND NOT APPLE)
  # When building a wheel, the Python extension is relocated into site-packages.
  # Install the shared-library dependencies into `zimg/lib` so that `RPATH=$ORIGIN/lib`
  # keeps working after relocation.
  #
  # Use the SONAME-level filenames (e.g. `.so.6`) to avoid relying on symlinks inside wheels.
  set(freeimagelibname libfreeimageplus.so.3)
  if (NOT ZIMG_DISABLE_FREEIMAGE)
    add_custom_command(TARGET ${module_name}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy ${FREEIMAGE_LIBRARIES}
                       $<TARGET_FILE_DIR:${module_name}>/${freeimagelibname}
    )
  endif ()
  set(tbblibname libtbb.so.12)
  add_custom_command(TARGET ${module_name}
                     POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:TBB::tbb>
                     $<TARGET_FILE_DIR:${module_name}>/${tbblibname}
  )
  set(qtcorelibname libQt6Core.so.6)
  add_custom_command(TARGET ${module_name}
                     POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:Qt6::Core>
                     $<TARGET_FILE_DIR:${module_name}>/${qtcorelibname}
  )
  set(qtguilibname libQt6Gui.so.6)
  add_custom_command(TARGET ${module_name}
                     POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:Qt6::Gui>
                     $<TARGET_FILE_DIR:${module_name}>/${qtguilibname}
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
  if (NOT ZIMG_DISABLE_FREEIMAGE)
    install(FILES ${FREEIMAGE_DLLS} DESTINATION zimg)
  endif ()
  install(FILES $<TARGET_FILE:TBB::tbb> DESTINATION zimg)
  install(FILES $<TARGET_FILE:Qt6::Core> DESTINATION zimg)
  install(FILES $<TARGET_FILE:Qt6::Gui> DESTINATION zimg)
endif (WIN32)

if (APPLE)
  if (NOT ZIMG_DISABLE_FREEIMAGE)
    install(FILES $<TARGET_FILE_DIR:${module_name}>/${freeimagelibname} DESTINATION zimg/lib)
  endif ()
  install(FILES $<TARGET_FILE_DIR:${module_name}>/${tbblibname} DESTINATION zimg/lib)
  install(FILES $<TARGET_FILE:Qt6::Core> DESTINATION zimg/lib/QtCore.framework/Versions/A)
  install(FILES $<TARGET_FILE:Qt6::Gui> DESTINATION zimg/lib/QtGui.framework/Versions/A)
  install(FILES $<TARGET_FILE:Qt6::DBus> DESTINATION zimg/lib/QtDBus.framework/Versions/A)

  option(IMG_PY_STRIP_INSTALLED_MACHO
         "Strip debug symbols from installed Mach-O binaries on macOS (must run before codesign)."
         ${IMG_PY_STRIP_INSTALLED_BINARIES})
  if (IMG_PY_STRIP_INSTALLED_MACHO)
    set(_img_py_strip_tool "${_IMG_PY_STRIP_TOOL}")
    set(_img_py_strip_install_code_template [[
      set(_install_root "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
      set(_strip_tool "@_img_py_strip_tool@")
      if(_strip_tool STREQUAL "")
        message(FATAL_ERROR "strip tool not configured (CMAKE_STRIP/strip not found).")
      endif()

      file(GLOB _strip_targets
        "${_install_root}/zimg/_imgpy*.so"
        "${_install_root}/zimg/lib/*.dylib"
        "${_install_root}/zimg/lib/*.framework/Versions/*/*"
      )
      list(REMOVE_DUPLICATES _strip_targets)

      foreach(_path IN LISTS _strip_targets)
        if(NOT EXISTS "${_path}")
          continue()
        endif()

        execute_process(
          COMMAND "${_strip_tool}" -S "${_path}"
          RESULT_VARIABLE _strip_rc
          OUTPUT_VARIABLE _strip_out
          ERROR_VARIABLE _strip_err
        )
        if(NOT _strip_rc EQUAL 0)
          message(FATAL_ERROR "strip failed for ${_path} (exit ${_strip_rc})\n${_strip_out}\n${_strip_err}")
        endif()
      endforeach()
    ]])
    string(CONFIGURE "${_img_py_strip_install_code_template}" _img_py_strip_install_code @ONLY)
    install(CODE "${_img_py_strip_install_code}")
  endif ()

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

if (UNIX AND NOT APPLE)
  if (NOT ZIMG_DISABLE_FREEIMAGE)
    install(FILES $<TARGET_FILE_DIR:${module_name}>/${freeimagelibname} DESTINATION zimg/lib)
  endif ()
  install(FILES $<TARGET_FILE_DIR:${module_name}>/${tbblibname} DESTINATION zimg/lib)
  install(FILES $<TARGET_FILE_DIR:${module_name}>/${qtcorelibname} DESTINATION zimg/lib)
  install(FILES $<TARGET_FILE_DIR:${module_name}>/${qtguilibname} DESTINATION zimg/lib)

  if (IMG_PY_STRIP_INSTALLED_BINARIES)
    set(_img_py_strip_tool "${_IMG_PY_STRIP_TOOL}")
    set(_img_py_strip_install_code_template [[
      set(_install_root "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
      set(_strip_tool "@_img_py_strip_tool@")
      if(_strip_tool STREQUAL "")
        message(FATAL_ERROR "strip tool not configured (CMAKE_STRIP/strip not found).")
      endif()

      file(GLOB _strip_targets
        "${_install_root}/zimg/_imgpy*.so"
        "${_install_root}/zimg/lib/*.so*"
      )
      list(REMOVE_DUPLICATES _strip_targets)

      foreach(_path IN LISTS _strip_targets)
        if(NOT EXISTS "${_path}")
          continue()
        endif()

        execute_process(
          COMMAND "${_strip_tool}" --strip-debug "${_path}"
          RESULT_VARIABLE _strip_rc
          OUTPUT_VARIABLE _strip_out
          ERROR_VARIABLE _strip_err
        )
        if(NOT _strip_rc EQUAL 0)
          message(FATAL_ERROR "strip failed for ${_path} (exit ${_strip_rc})\n${_strip_out}\n${_strip_err}")
        endif()
      endforeach()
    ]])
    string(CONFIGURE "${_img_py_strip_install_code_template}" _img_py_strip_install_code @ONLY)
    install(CODE "${_img_py_strip_install_code}")
  endif ()
endif ()

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/jars" # source directory
        DESTINATION zimg # target directory (relative so --prefix/DESTDIR work)
        FILES_MATCHING PATTERN "*.jar"
        PATTERN "*idea*" EXCLUDE
        PATTERN "*cmake*" EXCLUDE
)
