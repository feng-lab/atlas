# Resolve host SDK locations through Atlas' shared CMake logic so direct IDE
# configures and Python-driven builds use the exact same Qt/Vulkan selection.
include(AtlasHostSdk)
atlas_resolve_host_sdks()
if (NOT INTEL_PATH)
  if (WIN32)
    set(ATLAS_PIP_INTEL_PATH "${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/oneapi-pip/Library")
    set(ATLAS_STANDARD_INTEL_PATH "C:\\Program Files (x86)\\Intel\\oneAPI")
    set(INTEL_PATH "${ATLAS_PIP_INTEL_PATH}")
    if (NOT EXISTS "${INTEL_PATH}/lib/mkl_core.lib" OR NOT EXISTS "${INTEL_PATH}/lib/cmake/tbb/TBBConfig.cmake")
      set(INTEL_PATH "${ATLAS_STANDARD_INTEL_PATH}")
    endif ()
  elseif (APPLE)
    set(ATLAS_BUNDLED_INTEL_PATH "${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/oneapi")
    set(ATLAS_STANDARD_INTEL_PATH /opt/intel/oneapi)
    set(INTEL_PATH "${ATLAS_BUNDLED_INTEL_PATH}")
    if (NOT EXISTS "${INTEL_PATH}/mkl/latest")
      set(INTEL_PATH "${ATLAS_STANDARD_INTEL_PATH}")
    endif ()
  else (WIN32)
    set(ATLAS_PIP_INTEL_PATH "${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/oneapi-pip")
    set(ATLAS_STANDARD_INTEL_PATH /opt/intel/oneapi)
    set(INTEL_PATH "${ATLAS_PIP_INTEL_PATH}")
    if (NOT EXISTS "${INTEL_PATH}/lib/libmkl_core.a" OR NOT EXISTS "${INTEL_PATH}/lib/cmake/tbb/TBBConfig.cmake")
      set(INTEL_PATH "${ATLAS_STANDARD_INTEL_PATH}")
    endif ()
  endif (WIN32)
endif ()
message(STATUS "INTEL_PATH: ${INTEL_PATH}")
if (WIN32 AND EXISTS "${INTEL_PATH}/lib/mkl_core.lib" AND EXISTS "${INTEL_PATH}/lib/cmake/tbb/TBBConfig.cmake")
  set(ATLAS_PIP_INTEL_LAYOUT ON)
elseif (NOT APPLE AND EXISTS "${INTEL_PATH}/lib/libmkl_core.a" AND EXISTS "${INTEL_PATH}/lib/cmake/tbb/TBBConfig.cmake")
  set(ATLAS_PIP_INTEL_LAYOUT ON)
else ()
  set(ATLAS_PIP_INTEL_LAYOUT OFF)
endif ()
if (ATLAS_PIP_INTEL_LAYOUT)
  set(TBB_DIR ${INTEL_PATH}/lib/cmake/tbb)
elseif (NOT APPLE)
  set(TBB_DIR ${INTEL_PATH}/tbb/latest/lib/cmake/tbb)
endif ()
set(QT_HOST_PATH_CMAKE_DIR ${QT_HOST_PATH}/lib/cmake)

find_package(TBB REQUIRED tbb)
print_target_properties(TBB::tbb)


  if (ATLAS_PIP_INTEL_LAYOUT)
    set(MKL_PATH "${INTEL_PATH}")
  elseif (WIN32)
    set(MKL_PATH "${INTEL_PATH}\\mkl\\latest")
  else (WIN32)
    set(MKL_PATH ${INTEL_PATH}/mkl/latest)
  endif ()
  set(MKL_INCLUDE_DIRS ${MKL_INCLUDE_DIRS} ${MKL_PATH}/include)
  if (WIN32)
    set(MKL_LIBRARIES ${MKL_LIBRARIES}
        ${MKL_PATH}/lib/mkl_intel_lp64.lib
        ${MKL_PATH}/lib/mkl_tbb_thread.lib
        ${MKL_PATH}/lib/mkl_core.lib)
  elseif (APPLE)
    set(MKL_LIBRARIES ${MKL_LIBRARIES}
        ${MKL_PATH}/lib/libmkl_intel_lp64.a
        ${MKL_PATH}/lib/libmkl_tbb_thread.a
        ${MKL_PATH}/lib/libmkl_core.a)
  elseif (ATLAS_PIP_INTEL_LAYOUT)
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
find_package(libjpeg-turbo CONFIG REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
if (TARGET libjpeg-turbo::jpeg-static AND NOT TARGET libjpeg-turbo::jpeg)
  add_library(libjpeg-turbo::jpeg INTERFACE IMPORTED)
  set_property(TARGET libjpeg-turbo::jpeg PROPERTY INTERFACE_LINK_LIBRARIES libjpeg-turbo::jpeg-static)
endif ()
if (TARGET libjpeg-turbo::jpeg AND NOT TARGET JPEG::JPEG)
  add_library(JPEG::JPEG INTERFACE IMPORTED)
  set_property(TARGET JPEG::JPEG PROPERTY INTERFACE_LINK_LIBRARIES libjpeg-turbo::jpeg)
endif ()
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
print_target_properties(libjpeg-turbo::jpeg)

# Our vendored zlib build is static-only. On Windows upstream zlib names that
# archive `zs.lib`, which CMake's FindZLIB searches only in static mode.
set(ZLIB_USE_STATIC_LIBS ON)
find_package(ZLIB MODULE REQUIRED)
print_target_properties(ZLIB::ZLIB)

# libpng16.cmake does not provide include dir, so we have to create it, note PNG_INCLUDE_DIRS will only used by 1 file
include(${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/libpng/libpng16.cmake)
set(PNG_INCLUDE_DIRS ${PNG_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/include/libpng16)
set(PNG_LIBRARY ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/libpng.a)
set(PNG_PNG_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/include)
if (TARGET png_static AND NOT TARGET PNG::PNG)
  add_library(PNG::PNG INTERFACE IMPORTED)
  set_property(TARGET PNG::PNG PROPERTY INTERFACE_LINK_LIBRARIES png_static)
endif ()
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
if (TARGET zstd::libzstd_static AND NOT TARGET ZSTD::ZSTD)
  add_library(ZSTD::ZSTD ALIAS zstd::libzstd_static)
endif ()

if (NOT TARGET CMath::CMath)
  add_library(CMath::CMath INTERFACE IMPORTED)
  find_library(CMATH_LIBRARY m)
  if (CMATH_LIBRARY)
    set_property(TARGET CMath::CMath PROPERTY INTERFACE_LINK_LIBRARIES "${CMATH_LIBRARY}")
  endif ()
endif ()

find_package(Tiff CONFIG REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
if (TARGET TIFF::tiff AND NOT TARGET TIFF::TIFF)
  add_library(TIFF::TIFF ALIAS TIFF::tiff)
endif ()
if (TARGET TIFF::tiffxx AND NOT TARGET TIFF::CXX)
  add_library(TIFF::CXX ALIAS TIFF::tiffxx)
endif ()
print_target_properties(TIFF::tiff)
print_target_properties(TIFF::CXX)

set(_ATLAS_OPENIMAGEIO_PREFIX ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build)
set(_ATLAS_PREVIOUS_CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH})
list(PREPEND CMAKE_PREFIX_PATH ${_ATLAS_OPENIMAGEIO_PREFIX})
if (APPLE)
  set(_ATLAS_PREVIOUS_CMAKE_FIND_FRAMEWORK ${CMAKE_FIND_FRAMEWORK})
  set(CMAKE_FIND_FRAMEWORK LAST)
endif ()

find_package(Imath CONFIG REQUIRED
             PATHS ${_ATLAS_OPENIMAGEIO_PREFIX} NO_DEFAULT_PATH)
find_package(OpenEXR CONFIG REQUIRED
             PATHS ${_ATLAS_OPENIMAGEIO_PREFIX} NO_DEFAULT_PATH)
find_package(OpenColorIO CONFIG REQUIRED
             PATHS ${_ATLAS_OPENIMAGEIO_PREFIX} NO_DEFAULT_PATH)
find_package(OpenJPEG CONFIG REQUIRED
             PATHS ${_ATLAS_OPENIMAGEIO_PREFIX} NO_DEFAULT_PATH)
find_package(fmt CONFIG REQUIRED
             PATHS ${_ATLAS_OPENIMAGEIO_PREFIX} NO_DEFAULT_PATH)
find_package(OpenImageIO CONFIG REQUIRED
             PATHS ${_ATLAS_OPENIMAGEIO_PREFIX} NO_DEFAULT_PATH)
print_target_properties(OpenColorIO::OpenColorIO)
print_target_properties(fmt::fmt)
print_target_properties(fmt::fmt-header-only)
print_target_properties(OpenImageIO::OpenImageIO)

set(CMAKE_PREFIX_PATH ${_ATLAS_PREVIOUS_CMAKE_PREFIX_PATH})
if (APPLE)
  set(CMAKE_FIND_FRAMEWORK ${_ATLAS_PREVIOUS_CMAKE_FIND_FRAMEWORK})
endif ()
unset(_ATLAS_PREVIOUS_CMAKE_FIND_FRAMEWORK)
unset(_ATLAS_PREVIOUS_CMAKE_PREFIX_PATH)
unset(_ATLAS_OPENIMAGEIO_PREFIX)

find_package(ITK REQUIRED
             COMPONENTS ITKIOMeta ITKIONIFTI ITKIONRRD ITKIOGDCM ITKBinaryMathematicalMorphology ITKSmoothing
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
print_target_properties(ITKSmoothing)

find_package(gflags REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(gflags)
print_target_properties(gflags_static)

find_package(glog REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(glog::glog)

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

find_package(Protobuf CONFIG REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
message(STATUS "Using Protobuf ${Protobuf_VERSION}")
print_target_properties(protobuf::libprotobuf)
print_target_properties(protobuf::protoc)

find_package(gRPC CONFIG REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
message(STATUS "Using gRPC ${gRPC_VERSION}")
print_target_properties(gRPC::grpc++)
print_target_properties(gRPC::grpc_cpp_plugin)

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

find_package(Qt6 ${QT_VERSION} REQUIRED COMPONENTS Core PATHS ${QT_HOST_PATH} NO_DEFAULT_PATH)
print_target_properties(Qt6::Core)
set(_QT_LIBS_ Qt6::Core)

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
print_target_properties(VTK::sqlite)

find_package(llfio
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
if (llfio_FOUND)
  print_target_properties(llfio::sl)
else ()
  message(STATUS "no llfio")
endif ()
