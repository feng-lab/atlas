if (BUILD_WITH_CONDA)
  # qt
  set(QT_PATHS $ENV{PREFIX})

  # tbb
  include(${CMAKE_CURRENT_LIST_DIR}/../cmake/tbb/TBBMakeConfig.cmake)
  FIND_PATH(TBBROOT include/tbb/task_scheduler_init.h
            DOC "Root of TBB installation"
            HINTS ${TBBROOT}
            PATHS
            $ENV{PREFIX}
            )
  tbb_make_config(TBB_ROOT ${TBBROOT} SAVE_TO ${CMAKE_CURRENT_LIST_DIR}/../python/cmake)
  # tbb
  set(TBB_DIR ${CMAKE_CURRENT_LIST_DIR}/../python/cmake)
else ()
  # qt
  include(${CMAKE_CURRENT_LIST_DIR}/QtInfo.cmake)
  # tbb
  set(TBB_DIR ${CMAKE_CURRENT_LIST_DIR})
endif ()

find_package(TBB REQUIRED tbb)
# get_target_property(TBB_INCLUDE_DIRS TBB::tbb INTERFACE_INCLUDE_DIRECTORIES)
# message(STATUS "TBB_INCLUDE_DIRS:" ${TBB_INCLUDE_DIRS})
get_target_property(TBB_LIBRARY TBB::tbb IMPORTED_LOCATION_RELEASE)
message(STATUS "TBB_LIBRARY: ${TBB_LIBRARY}")
get_target_property(TBB_IMPORTED_IMPLIB_RELEASE TBB::tbb IMPORTED_IMPLIB_RELEASE)
message(STATUS "TBB_IMPORTED_IMPLIB_RELEASE: ${TBB_IMPORTED_IMPLIB_RELEASE}")

if (BUILD_WITH_CONDA)
  set(MKL_INCLUDE_DIRS ${MKL_INCLUDE_DIRS} $ENV{PREFIX}/include $ENV{PREFIX}/include/fftw)
  find_library(MKL_INTEL_LP64 NAMES mkl_intel_lp64
               PATHS $ENV{PREFIX}/lib NO_DEFAULT_PATH)
  find_library(MKL_TBB_THREAD NAMES mkl_tbb_thread
               PATHS $ENV{PREFIX}/lib NO_DEFAULT_PATH)
  find_library(MKL_CORE NAMES mkl_core
               PATHS $ENV{PREFIX}/lib NO_DEFAULT_PATH)
  set(MKL_LIBRARIES ${MKL_INTEL_LP64} ${MKL_TBB_THREAD} ${MKL_CORE})
  message(STATUS "MKL_INCLUDE_DIRS: ${MKL_INCLUDE_DIRS}")
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

find_package(ITK REQUIRED COMPONENTS ITKIOMeta ITKIONIFTI ITKIONRRD ITKIOGDCM ITKBinaryMathematicalMorphology
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/itk NO_DEFAULT_PATH)
message(STATUS "ITK_DIR: ${ITK_DIR}")
message(STATUS "ITK_USE_FILE: ${ITK_USE_FILE}")
include(${ITK_USE_FILE})
message(STATUS "ITK_INCLUDE_DIRS: ${ITK_INCLUDE_DIRS}")
message(STATUS "ITK_LIBRARIES: ${ITK_LIBRARIES}")

set(CPUINFO_INCLUDE_DIRS ${CPUINFO_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/cpuinfo/include)
if (WIN32)
  set(CPUINFO_LIBRARIES ${CPUINFO_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/cpuinfo/lib/cpuinfo.lib
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/cpuinfo/lib/clog.lib)
else (WIN32)
  set(CPUINFO_LIBRARIES ${CPUINFO_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/cpuinfo/lib/libcpuinfo.a
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/cpuinfo/lib/libclog.a)
endif (WIN32)

#find_package(libjpeg REQUIRED PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/libjpeg-turbo NO_DEFAULT_PATH)
set(JPEGTURBO_INCLUDE_DIRS ${JPEGTURBO_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/libjpeg-turbo/include)
if (WIN32)
  set(JPEGTURBO_LIBRARIES ${JPEGTURBO_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/libjpeg-turbo/lib/jpeg-static.lib)
else (WIN32)
  set(JPEGTURBO_LIBRARIES ${JPEGTURBO_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/libjpeg-turbo/lib/libjpeg.a)
endif (WIN32)

# libpng16.cmake does not provide include dir, so we have to create it, note PNG_INCLUDE_DIRS will only used by 1 file
include(${CMAKE_CURRENT_LIST_DIR}/../3rdparty/libpng/lib/libpng/libpng16.cmake)
set(PNG_INCLUDE_DIRS ${PNG_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/libpng/include)
#if (WIN32)
#  set(PNG_LIBRARIES ${PNG_LIBRARIES}
#      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/libpng/lib/png-static.lib)
#else (WIN32)
#  set(PNG_LIBRARIES ${PNG_LIBRARIES}
#      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/libpng/lib/libpng.a)
#endif (WIN32)
#find_package(libpng16 REQUIRED PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/libpng/lib/libpng NO_DEFAULT_PATH)

set(JPEGXR_INCLUDE_DIRS ${JPEGXR_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/jxrlib/include/libjxr/common
    ${JPEGXR_INCLUDE_DIRS} ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/jxrlib/include/libjxr/image/x86
    ${JPEGXR_INCLUDE_DIRS} ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/jxrlib/include/libjxr/image
    ${JPEGXR_INCLUDE_DIRS} ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/jxrlib/include/libjxr/glue)
if (WIN32)
  set(JPEGXR_LIBRARIES ${JPEGXR_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/jxrlib/lib/JXRGlueLib.lib
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/jxrlib/lib/JXRDecodeLib.lib
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/jxrlib/lib/JXREncodeLib.lib
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/jxrlib/lib/JXRCommonLib.lib)
else (WIN32)
  set(JPEGXR_LIBRARIES ${JPEGXR_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/jxrlib/lib/libjxrglue.a
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/jxrlib/lib/libjpegxr.a)
endif (WIN32)

if (WIN32)
  set(FREEIMAGE_INCLUDE_DIRS ${FREEIMAGE_INCLUDE_DIRS}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/freeimage)
  set(FREEIMAGE_LIBRARIES ${FREEIMAGE_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/freeimage/FreeImagePlus.lib
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/freeimage/FreeImage.lib)
elseif (APPLE)
  set(FREEIMAGE_INCLUDE_DIRS ${FREEIMAGE_INCLUDE_DIRS}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/freeimage/include)
  set(FREEIMAGE_LIBRARIES ${FREEIMAGE_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/freeimage/lib/libfreeimageplus.dylib)
else ()
  set(FREEIMAGE_INCLUDE_DIRS ${FREEIMAGE_INCLUDE_DIRS}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/freeimage/include)
  set(FREEIMAGE_LIBRARIES ${FREEIMAGE_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/freeimage/lib/libfreeimageplus.so)
endif ()

find_package(gflags REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/gflags NO_DEFAULT_PATH)
find_package(glog REQUIRED
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/glog ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/gflags
             NO_DEFAULT_PATH)

set(GEOMETRICTOOLS_INCLUDE_DIRS ${GEOMETRICTOOLS_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/geometrictools/include)
if (WIN32)
  set(GEOMETRICTOOLS_LIBRARIES ${GEOMETRICTOOLS_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/geometrictools/lib/GTEngine.v15.lib)
elseif (APPLE)
  set(GEOMETRICTOOLS_LIBRARIES ${GEOMETRICTOOLS_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/geometrictools/lib/libGTEngine.a)
else ()
  set(GEOMETRICTOOLS_LIBRARIES ${GEOMETRICTOOLS_LIBRARIES}
      ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/geometrictools/lib/libgtengine.a)
endif ()

find_package(HDF5 REQUIRED COMPONENTS C CXX static
             PATHS ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/hdf5/share/cmake/hdf5 ${CMAKE_CURRENT_LIST_DIR}/../3rdparty/hdf5/cmake NO_DEFAULT_PATH)

message(STATUS "QT_PATHS: " ${QT_PATHS})
set(CMAKE_AUTOMOC ON)
if (BUILD_WITH_CONDA)
  # find_package(Qt5Core ${QT_VERSION} REQUIRED PATHS ${QT_PATHS} NO_DEFAULT_PATH)
  find_package(Qt5 ${QT_VERSION} REQUIRED COMPONENTS Core Gui PATHS ${QT_PATHS} NO_DEFAULT_PATH)
else ()
  # find_package(Qt5Core ${QT_VERSION} REQUIRED PATHS ${QT_PATHS})
  find_package(Qt5 ${QT_VERSION} REQUIRED COMPONENTS Core Gui PATHS ${QT_PATHS})
endif ()
get_target_property(Qt5CoreLoc Qt5::Core IMPORTED_LOCATION_RELEASE)
message(STATUS "Qt5Core: ${Qt5CoreLoc}")
