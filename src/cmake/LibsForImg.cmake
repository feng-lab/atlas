if (BUILD_WITH_CONDA)
  # qt
  set(QT_HOST_PATH $ENV{PREFIX})
  # tbb
  set(TBB_DIR $ENV{PREFIX}/lib/cmake/tbb)
else ()
  # qt
  include(${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/QtInfo.cmake)
  # tbb
  # set(TBB_DIR ${CMAKE_CURRENT_LIST_DIR})
endif ()

find_package(TBB REQUIRED tbb)
print_target_properties(TBB::tbb)

if (BUILD_WITH_CONDA)
  set(MKL_INCLUDE_DIRS ${MKL_INCLUDE_DIRS} $ENV{PREFIX}/include $ENV{PREFIX}/include/fftw)
  find_library(MKL_INTEL_LP64 NAMES mkl_intel_lp64
               PATHS $ENV{PREFIX}/lib NO_DEFAULT_PATH)
  find_library(MKL_TBB_THREAD NAMES mkl_tbb_thread
               PATHS $ENV{PREFIX}/lib NO_DEFAULT_PATH)
  find_library(MKL_CORE NAMES mkl_core
               PATHS $ENV{PREFIX}/lib NO_DEFAULT_PATH)
  set(MKL_LIBRARIES ${MKL_INTEL_LP64} ${MKL_TBB_THREAD} ${MKL_CORE})
else ()
  if (WIN32)
    set(INTEL_PATH "C:\\Program Files (x86)\\IntelSWTools\\compilers_and_libraries\\windows\\compiler")
    set(MKL_PATH "C:\\Program Files (x86)\\IntelSWTools\\compilers_and_libraries\\windows\\mkl")
  else (WIN32)
    set(INTEL_PATH /opt/intel)
    set(MKL_PATH ${INTEL_PATH}/mkl)
  endif (WIN32)
  set(MKL_INCLUDE_DIRS ${MKL_INCLUDE_DIRS} ${MKL_PATH}/include ${MKL_PATH}/include/fftw)
  if (WIN32)
    # todo: fix, mkl_tbb_thread links to static version of msvc runtime so we can not use it now
    set(MKL_LIBRARIES ${MKL_LIBRARIES}
        ${MKL_PATH}/lib/intel64/mkl_intel_lp64.lib
        ${MKL_PATH}/lib/intel64/mkl_sequential.lib
        ${MKL_PATH}/lib/intel64/mkl_core.lib)
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

  # ipp
  if (WIN32)
    set(IPP_PATH "C:\\Program Files (x86)\\IntelSWTools\\compilers_and_libraries\\windows\\ipp")
  else (WIN32)
    set(INTEL_PATH /opt/intel)
    set(IPP_PATH ${INTEL_PATH}/ipp)
  endif (WIN32)
  set(IPP_INCLUDE_DIRS ${IPP_INCLUDE_DIRS} ${IPP_PATH}/include)
  if (WIN32)
    set(IPP_LIBRARIES ${IPP_LIBRARIES}
        ${IPP_PATH}/lib/intel64/ippimt.lib
        ${IPP_PATH}/lib/intel64/ippcoremt.lib
        ${IPP_PATH}/lib/intel64/ippvmmt.lib
        ${IPP_PATH}/lib/intel64/ippsmt.lib
        ${IPP_PATH}/lib/intel64/ippcvmt.lib
        ${IPP_PATH}/lib/intel64/ippccmt.lib)
  elseif (APPLE)
    set(IPP_LIBRARIES ${IPP_LIBRARIES}
        ${IPP_PATH}/lib/libippi.a
        ${IPP_PATH}/lib/libippcore.a
        ${IPP_PATH}/lib/libippvm.a
        #${IPP_PATH}/lib/libipps.a
        ${IPP_PATH}/lib/libippcv.a
        ${IPP_PATH}/lib/libippcc.a
        ${INTEL_PATH}/lib/libirc.a
        ${INTEL_PATH}/lib/libsvml.a
        ${INTEL_PATH}/lib/libimf.a)
  else ()
    set(IPP_LIBRARIES ${IPP_LIBRARIES}
        ${IPP_PATH}/lib/intel64/libippi.a
        ${IPP_PATH}/lib/intel64/libippcore.a
        ${IPP_PATH}/lib/intel64/libippvm.a
        ${IPP_PATH}/lib/intel64/libipps.a
        ${IPP_PATH}/lib/intel64/libippcv.a
        ${IPP_PATH}/lib/intel64/libippcc.a)
  endif ()
endif ()
message(STATUS "MKL_INCLUDE_DIRS: ${MKL_INCLUDE_DIRS}")
message(STATUS "MKL_LIBRARIES: ${MKL_LIBRARIES}")
message(STATUS "IPP_INCLUDE_DIRS: ${IPP_INCLUDE_DIRS}")
message(STATUS "IPP_LIBRARIES: ${IPP_LIBRARIES}")

set(CPUINFO_INCLUDE_DIRS ${CPUINFO_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/include)
if (WIN32)
  set(CPUINFO_LIBRARIES ${CPUINFO_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/cpuinfo.lib
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/clog.lib)
else (WIN32)
  set(CPUINFO_LIBRARIES ${CPUINFO_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/libcpuinfo.a
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/lib/libclog.a)
endif (WIN32)
message(STATUS "CPUINFO_INCLUDE_DIRS: ${CPUINFO_INCLUDE_DIRS}")
message(STATUS "CPUINFO_LIBRARIES: ${CPUINFO_LIBRARIES}")

#find_package(libjpeg REQUIRED PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/libjpeg-turbo NO_DEFAULT_PATH)
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
#if (WIN32)
#  set(PNG_LIBRARIES ${PNG_LIBRARIES}
#      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/libpng/lib/png-static.lib)
#else (WIN32)
#  set(PNG_LIBRARIES ${PNG_LIBRARIES}
#      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/libpng/lib/libpng.a)
#endif (WIN32)
#find_package(libpng16 REQUIRED PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/libpng/lib/libpng NO_DEFAULT_PATH)
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

if (WIN32)
  set(FREEIMAGE_INCLUDE_DIRS ${FREEIMAGE_INCLUDE_DIRS}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/freeimage)
  set(FREEIMAGE_LIBRARIES ${FREEIMAGE_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/freeimage/FreeImagePlus.lib
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build/freeimage/FreeImage.lib)
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

find_package(WebP REQUIRED
             COMPONENTS webp
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(WebP::webp)

find_package(liblzma REQUIRED
             COMPONENTS liblzma
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(liblzma::liblzma)

find_package(zstd REQUIRED
             COMPONENTS libzstd_static
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

find_package(gflags REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(gflags)
find_package(glog REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build
             NO_DEFAULT_PATH)
print_target_properties(glog::glog)

add_library(GeometricTools INTERFACE IMPORTED)
set_target_properties(GeometricTools PROPERTIES
                      INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../3rdparty/GeometricTools/GTE")
print_target_properties(GeometricTools)

add_library(glm::glm INTERFACE IMPORTED)
set_target_properties(glm::glm PROPERTIES
                      INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../3rdparty/glm")
print_target_properties(glm::glm)

add_library(range-v3::range-v3 INTERFACE IMPORTED)
set_target_properties(range-v3::range-v3 PROPERTIES
                      INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../3rdparty/range-v3/include")
print_target_properties(range-v3::range-v3)

find_package(HDF5 REQUIRED
             COMPONENTS C CXX static
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(hdf5_cpp-static)
print_target_properties(hdf5-static)

find_package(Ceres REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(Eigen3::Eigen)
print_target_properties(Ceres::ceres)

if (${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION} LESS 3.17)
  set(Boost_USE_STATIC_LIBS ON)
  find_package(Boost 1.73.0 REQUIRED
               COMPONENTS
               headers context filesystem program_options regex system thread
               PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH
               )
  print_target_properties(Boost::headers)
else (${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION} LESS 3.17)
  set(Boost_USE_STATIC_LIBS ON)
  find_package(Boost 1.73.0 REQUIRED
               COMPONENTS
               headers
               PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH
               )
  print_target_properties(Boost::headers)
endif (${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION} LESS 3.17)

find_package(folly REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(Boost::context)
print_target_properties(Boost::filesystem)
print_target_properties(Boost::program_options)
print_target_properties(Boost::regex)
print_target_properties(Boost::system)
print_target_properties(Boost::thread)
print_target_properties(Folly::folly)
print_target_properties(Folly::folly_deps)
print_target_properties(fmt::fmt)
print_target_properties(Threads::Threads)

message(STATUS "QT_HOST_PATH: " ${QT_HOST_PATH})
message(STATUS "QT_VERSION: " ${QT_VERSION})
set(CMAKE_AUTOMOC ON)
if (BUILD_WITH_CONDA)
  find_package(Qt5 ${QT_VERSION} REQUIRED COMPONENTS Core Gui PATHS ${QT_HOST_PATH} NO_DEFAULT_PATH)
  print_target_properties(Qt5::Core)
  print_target_properties(Qt5::Gui)
  set(_QT_LIBS_ Qt5::Core Qt5::Gui)
else ()
  find_package(Qt6 ${QT_VERSION} REQUIRED COMPONENTS Core Gui PATHS ${QT_HOST_PATH} NO_DEFAULT_PATH)
  print_target_properties(Qt6::Core)
  print_target_properties(Qt6::Gui)
  set(_QT_LIBS_ Qt6::Core Qt6::Gui)
endif ()
