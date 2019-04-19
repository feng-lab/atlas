cmake_minimum_required(VERSION 3.6.1)
project(zimg)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.12)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# set_target_properties(MyTarget PROPERTIES CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

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

  set(CMAKE_INSTALL_RPATH "$ORIGIN/")
endif ()

if (UNIX)
  # Do not put non-global symbols in the output file's symbol table.
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-x")
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-x")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-x")
endif ()

message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")

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

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/LibsForImg.cmake)

# subprojects
add_subdirectory(../img condaimg)

# pybind11
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../3rdparty/pybind11 pybind11)
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
                       @loader_path/libfreeimageplus.dylib
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

# install
install(DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/" # source directory
        DESTINATION $ENV{SP_DIR}/zimg # target directory
        FILES_MATCHING PATTERN "*.py"
        PATTERN "*idea*" EXCLUDE
        PATTERN "*cmake*" EXCLUDE
        )
install(TARGETS ${module_name} DESTINATION $ENV{SP_DIR}/zimg)
install(FILES $<TARGET_FILE_DIR:${module_name}>/${freeimagelibname} DESTINATION $ENV{SP_DIR}/zimg)
