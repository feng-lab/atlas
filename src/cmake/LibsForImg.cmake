# qt
include(${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/PathList.cmake)
if (NOT INTEL_PATH)
  if (WIN32)
    set(INTEL_PATH "C:\\Program Files (x86)\\Intel\\oneAPI")
  else (WIN32)
    set(INTEL_PATH /opt/intel/oneapi)
  endif (WIN32)
endif ()
message(STATUS "INTEL_PATH: ${INTEL_PATH}")
if (NOT APPLE)
  # tbb
  set(TBB_DIR ${INTEL_PATH}/tbb/latest/lib/cmake/tbb)
endif ()
set(QT_HOST_PATH_CMAKE_DIR ${QT_HOST_PATH}/lib/cmake)

find_package(TBB REQUIRED tbb)
print_target_properties(TBB::tbb)


  if (WIN32)
    set(MKL_PATH "${INTEL_PATH}\\mkl\\latest")
  else (WIN32)
    set(MKL_PATH ${INTEL_PATH}/mkl/latest)
  endif (WIN32)
  set(MKL_INCLUDE_DIRS ${MKL_INCLUDE_DIRS} ${MKL_PATH}/include)
  if (WIN32)
    # todo: fix, mkl_tbb_thread links to static version of msvc runtime so we can not use it now
    set(MKL_LIBRARIES ${MKL_LIBRARIES}
        ${MKL_PATH}/lib/mkl_intel_lp64.lib
        ${MKL_PATH}/lib/mkl_sequential.lib
        ${MKL_PATH}/lib/mkl_core.lib)
  elseif (APPLE)
    set(MKL_LIBRARIES ${MKL_LIBRARIES}
        ${MKL_PATH}/lib/libmkl_intel_lp64.a
        ${MKL_PATH}/lib/libmkl_tbb_thread.a
        ${MKL_PATH}/lib/libmkl_core.a)
  else ()
    set(MKL_LIBRARIES ${MKL_LIBRARIES}
        ${MKL_PATH}/lib/intel64/libmkl_intel_lp64.a
        ${MKL_PATH}/lib/intel64/libmkl_tbb_thread.a
        ${MKL_PATH}/lib/intel64/libmkl_core.a)
  endif ()

message(STATUS "MKL_INCLUDE_DIRS: ${MKL_INCLUDE_DIRS}")
message(STATUS "MKL_LIBRARIES: ${MKL_LIBRARIES}")

find_package(cpuinfo REQUIRED PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(cpuinfo::cpuinfo)

set(JPEGTURBO_INCLUDE_DIRS ${JPEGTURBO_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/include)
if (WIN32)
  set(JPEGTURBO_LIBRARIES ${JPEGTURBO_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/jpeg-static.lib
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/turbojpeg-static.lib)
else (WIN32)
  set(JPEGTURBO_LIBRARIES ${JPEGTURBO_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/libjpeg.a
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/libturbojpeg.a)
endif (WIN32)
message(STATUS "JPEGTURBO_INCLUDE_DIRS: ${JPEGTURBO_INCLUDE_DIRS}")
message(STATUS "JPEGTURBO_LIBRARIES: ${JPEGTURBO_LIBRARIES}")

find_package(ZLIB MODULE REQUIRED)
print_target_properties(ZLIB::ZLIB)

# libpng16.cmake does not provide include dir, so we have to create it, note PNG_INCLUDE_DIRS will only used by 1 file
include(${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/libpng/libpng16.cmake)
set(PNG_INCLUDE_DIRS ${PNG_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/include/libpng16)
message(STATUS "PNG_INCLUDE_DIRS: ${PNG_INCLUDE_DIRS}")
print_target_properties(png_static)

set(JPEGXR_INCLUDE_DIRS ${JPEGXR_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/include/libjxr/common
    ${JPEGXR_INCLUDE_DIRS} ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/include/libjxr/image/x86
    ${JPEGXR_INCLUDE_DIRS} ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/include/libjxr/image
    ${JPEGXR_INCLUDE_DIRS} ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/include/libjxr/glue)
if (WIN32)
  set(JPEGXR_LIBRARIES ${JPEGXR_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/JXRGlueLib.lib
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/JXRDecodeLib.lib
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/JXREncodeLib.lib
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/JXRCommonLib.lib)
else (WIN32)
  set(JPEGXR_LIBRARIES ${JPEGXR_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/libjxrglue.a
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/libjpegxr.a)
endif (WIN32)
message(STATUS "JPEGXR_INCLUDE_DIRS: ${JPEGXR_INCLUDE_DIRS}")
message(STATUS "JPEGXR_LIBRARIES: ${JPEGXR_LIBRARIES}")

if (ZIMG_DISABLE_FREEIMAGE)
  set(FREEIMAGE_INCLUDE_DIRS)
  set(FREEIMAGE_LIBRARIES)
  set(FREEIMAGE_DLLS)
  message(STATUS "FreeImage: disabled (ZIMG_DISABLE_FREEIMAGE=ON)")
else ()
  if (WIN32)
    set(FREEIMAGE_INCLUDE_DIRS ${FREEIMAGE_INCLUDE_DIRS}
        ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/freeimage)
    set(FREEIMAGE_LIBRARIES ${FREEIMAGE_LIBRARIES}
        ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/freeimage/FreeImagePlus.lib
        ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/freeimage/FreeImage.lib)
    set(FREEIMAGE_DLLS ${FREEIMAGE_DLLS}
        ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/freeimage/FreeImagePlus.dll
        ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/freeimage/FreeImage.dll)
  elseif (APPLE)
    set(FREEIMAGE_INCLUDE_DIRS ${FREEIMAGE_INCLUDE_DIRS}
        ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/include)
    set(FREEIMAGE_LIBRARIES ${FREEIMAGE_LIBRARIES}
        ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/libfreeimageplus.dylib)
  else ()
    set(FREEIMAGE_INCLUDE_DIRS ${FREEIMAGE_INCLUDE_DIRS}
        ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/include)
    set(FREEIMAGE_LIBRARIES ${FREEIMAGE_LIBRARIES}
        ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/libfreeimageplus.so)
  endif ()
  message(STATUS "FREEIMAGE_INCLUDE_DIRS: ${FREEIMAGE_INCLUDE_DIRS}")
  message(STATUS "FREEIMAGE_LIBRARIES: ${FREEIMAGE_LIBRARIES}")
endif ()

find_package(WebP REQUIRED
             COMPONENTS webp
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(WebP::webp)

find_package(liblzma REQUIRED
             COMPONENTS liblzma
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(liblzma::liblzma)

find_package(zstd REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(zstd::libzstd_static)

find_package(ITK REQUIRED
             COMPONENTS ITKIOMeta ITKIONIFTI ITKIONRRD ITKIOGDCM ITKBinaryMathematicalMorphology
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
message(STATUS "ITK_DIR: ${ITK_DIR}")
message(STATUS "ITK_USE_FILE: ${ITK_USE_FILE}")
include(${ITK_USE_FILE})
message(STATUS "ITK_INCLUDE_DIRS: ${ITK_INCLUDE_DIRS}")
message(STATUS "ITK_LIBRARIES: ${ITK_LIBRARIES}")
print_target_properties(ITKIOMeta)
print_target_properties(ITKIONIFTI)
print_target_properties(ITKIONRRD)
print_target_properties(ITKIOGDCM)
print_target_properties(ITKBinaryMathematicalMorphology)

# Config with namespace available since gflags 2.2.2
option(GFLAGS_USE_TARGET_NAMESPACE "Use gflags import target with namespace." ON)
find_package(gflags REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(gflags::gflags)
print_target_properties(gflags::gflags_static)

# Compatibility for fbcode-style exports:
# Some vendored packages (e.g. fizz/mvfst/proxygen) still link against the
# non-namespaced `gflags_static` target name. When we enable the namespace
# import targets, provide compatible shims so those link interfaces resolve to
# the same library.
if (GFLAGS_USE_TARGET_NAMESPACE)
  if (TARGET gflags::gflags_static AND NOT TARGET gflags_static)
    add_library(gflags_static INTERFACE)
    target_link_libraries(gflags_static INTERFACE gflags::gflags_static)
  endif ()
  if (TARGET gflags::gflags_nothreads_static AND NOT TARGET gflags_nothreads_static)
    add_library(gflags_nothreads_static INTERFACE)
    target_link_libraries(gflags_nothreads_static INTERFACE gflags::gflags_nothreads_static)
  endif ()
endif ()

find_package(glog REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(glog::glog)

add_library(GeometricTools INTERFACE IMPORTED)
set_target_properties(GeometricTools PROPERTIES
                      INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../3rdparty/GeometricTools/GTE")
print_target_properties(GeometricTools)

find_package(glm CONFIG REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/share NO_DEFAULT_PATH)
print_target_properties(glm::glm-header-only)

#add_library(range-v3::range-v3 INTERFACE IMPORTED)
#set_target_properties(range-v3::range-v3 PROPERTIES
#                      INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/range-v3/include")
#print_target_properties(range-v3::range-v3)

find_package(HDF5 REQUIRED
             COMPONENTS C CXX static
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/share NO_DEFAULT_PATH)
print_target_properties(hdf5_cpp-static)
print_target_properties(hdf5-static)

find_package(Eigen3 REQUIRED)
print_target_properties(Eigen3::Eigen)
find_package(absl REQUIRED PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
find_package(Ceres REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(Ceres::ceres)

set(Boost_USE_STATIC_LIBS ON)
find_package(Boost 1.89.0 REQUIRED
             COMPONENTS
             headers context filesystem program_options thread charconv
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH
)
print_target_properties(Boost::headers)

find_package(Snappy REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(Snappy::snappy)

find_package(lz4 REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(LZ4::lz4_static)

find_package(folly REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(Boost::context)
print_target_properties(Boost::filesystem)
print_target_properties(Boost::program_options)
print_target_properties(Boost::thread)
print_target_properties(Boost::charconv)
print_target_properties(Folly::folly)
print_target_properties(Folly::folly_deps)
print_target_properties(fmt::fmt)
print_target_properties(Threads::Threads)

message(STATUS "QT_HOST_PATH: " ${QT_HOST_PATH})
message(STATUS "QT_VERSION: " ${QT_VERSION})
set(CMAKE_AUTOMOC ON)

find_package(Qt6 ${QT_VERSION} REQUIRED COMPONENTS Core Gui PATHS ${QT_HOST_PATH} NO_DEFAULT_PATH)
print_target_properties(Qt6::Core)
print_target_properties(Qt6::Gui)
set(_QT_LIBS_ Qt6::Core Qt6::Gui)

find_package(assimp REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(assimp::assimp)
if (NOT TARGET assimp::draco)
  message(FATAL_ERROR "Atlas requires Draco for Neuroglancer mesh decoding, but the found Assimp package does not export 'assimp::draco'. Rebuild 3rdparty Assimp with Draco enabled (util/build_ext_libs.py assimp).")
endif ()
print_target_properties(assimp::draco)

find_package(VTK REQUIRED COMPONENTS FiltersGeometry FiltersSources IOXML FiltersModeling
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
message(STATUS "VTK_DIR: ${VTK_DIR}")
message(STATUS "VTK_LIBRARIES: ${VTK_LIBRARIES}")
print_target_properties(VTK::FiltersGeometry)
print_target_properties(VTK::FiltersSources)
print_target_properties(VTK::IOXML)

find_package(llfio
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
if (llfio_FOUND)
  print_target_properties(llfio::sl)
else ()
  message(STATUS "no llfio")
endif ()
