cmake_minimum_required(VERSION 3.6.1)
project(atlas)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.8)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake_modules)

if (NOT CMAKE_DEBUG_POSTFIX)
  set(CMAKE_DEBUG_POSTFIX d)
endif ()
message(${CMAKE_DEBUG_POSTFIX})

# Change the default build type from Debug to Release, while still
# supporting overriding the build type.
#
# The CACHE STRING logic here and elsewhere is needed to force CMake
# to pay attention to the value of these variables.
if (NOT CMAKE_BUILD_TYPE)
  message("-- No build type specified; defaulting to CMAKE_BUILD_TYPE=Release.")
  set(CMAKE_BUILD_TYPE Release CACHE STRING
    "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel."
    FORCE)
else (NOT CMAKE_BUILD_TYPE)
  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    message("\n=================================================================================")
    message("\n-- Build type: Debug. Performance will be terrible!")
    message("-- Add -DCMAKE_BUILD_TYPE=Release to the CMake command line to get an optimized build.")
    message("\n=================================================================================")
  endif (CMAKE_BUILD_TYPE STREQUAL "Debug")
endif (NOT CMAKE_BUILD_TYPE)

# main stuff

if (WIN32)
  set(INTEL_PATH "C:\Program Files (x86)\Intel\Composer XE\compiler")
  set(MKL_PATH "C:\Program Files (x86)\Intel\Composer XE\mkl")
else (WIN32)
  set(INTEL_PATH /opt/intel)
  set(MKL_PATH ${INTEL_PATH}/mkl)
endif (WIN32)
set(MKL_INCLUDE_DIRS ${MKL_INCLUDE_DIRS} ${MKL_PATH}/include ${MKL_PATH}/include/fftw)
if (WIN32)
  set(MKL_LIBRARIES ${MKL_LIBRARIES} ${MKL_PATH}/lib/intel64/mkl_intel_lp64.lib ${MKL_PATH}/lib/intel64/mkl_intel_thread.lib ${MKL_PATH}/lib/intel64/mkl_core.lib ${INTEL_PATH}/lib/intel64/libiomp5md.lib)
else (WIN32)
  set(MKL_LIBRARIES ${MKL_LIBRARIES} ${MKL_PATH}/lib/libmkl_intel_lp64.a ${MKL_PATH}/lib/libmkl_intel_thread.a ${MKL_PATH}/lib/libmkl_core.a ${INTEL_PATH}/lib/libiomp5.dylib)
endif (WIN32)

if (WIN32)
  set(IPP_PATH "C:\Program Files (x86)\Intel\Composer XE\ipp")
else (WIN32)
  set(IPP_PATH ${INTEL_PATH}/ipp)
endif (WIN32)
set(IPP_INCLUDE_DIRS ${IPP_INCLUDE_DIRS} ${IPP_PATH}/include)
if (WIN32)
  set(IPP_LIBRARIES ${IPP_LIBRARIES} ${IPP_PATH}/lib/intel64/ippimt.lib ${IPP_PATH}/lib/intel64/ippcoremt.lib ${IPP_PATH}/lib/intel64/ippvmmt.lib ${IPP_PATH}/lib/intel64/ippsmt.lib ${IPP_PATH}/lib/intel64/ippcvmt.lib ${IPP_PATH}/lib/intel64/ippccmt.lib ${INTEL_PATH}/lib/intel64/libirc.lib)
else (WIN32)
  set(IPP_LIBRARIES ${IPP_LIBRARIES} ${IPP_PATH}/lib/libippi.a ${IPP_PATH}/lib/libippcore.a ${IPP_PATH}/lib/libippvm.a ${IPP_PATH}/lib/libipps.a ${IPP_PATH}/lib/libippcv.a ${IPP_PATH}/lib/libippcc.a ${INTEL_PATH}/lib/libirc.a)
endif (WIN32)

find_package(VTK REQUIRED PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/vtk NO_DEFAULT_PATH)
message(${VTK_DIR})
message(${VTK_USE_FILE})
include(${VTK_USE_FILE})
message("${VTK_INCLUDE_DIRS}")
message("${VTK_LIBRARIES}")
message("${VTK_LIBRARY_DIRS}")

set(WILDMAGIC_INCLUDE_DIRS ${WILDMAGIC_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/wildmagic/include)
if (WIN32)
  set(WILDMAGIC_LIBRARIES ${WILDMAGIC_LIBRARIES} ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/wildmagic/lib/libGTEngine.a)
else (WIN32)
  set(WILDMAGIC_LIBRARIES ${WILDMAGIC_LIBRARIES} ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/wildmagic/lib/GTEngine.lib)
endif (WIN32)

find_package(HDF5 REQUIRED COMPONENTS C CXX PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/HDF5 NO_DEFAULT_PATH)
message(${HDF5_INCLUDE_DIRS})
message(${HDF5_CXX_LIBRARIES})
message(${HDF5_LIBRARIES})

find_package(ASSIMP REQUIRED PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/assimp NO_DEFAULT_PATH)
message(${ASSIMP_INCLUDE_DIRS})
message(${ASSIMP_LIBRARIES})

find_package(OPENCV REQUIRED PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/opencv NO_DEFAULT_PATH)

find_package(GLBINDING REQUIRED PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/glbinding NO_DEFAULT_PATH)







find_package(Qt5Core REQUIRED PATHS /Users/feng/Qt/5.7/clang_64 NO_DEFAULT_PATH)
find_package(Qt5Widgets REQUIRED PATHS /Users/feng/Qt/5.7/clang_64 NO_DEFAULT_PATH)
find_package(Qt5OpenGL REQUIRED PATHS /Users/feng/Qt/5.7/clang_64 NO_DEFAULT_PATH)
find_package(Qt5Network REQUIRED PATHS /Users/feng/Qt/5.7/clang_64 NO_DEFAULT_PATH)
find_package(Qt5SVG REQUIRED PATHS /Users/feng/Qt/5.7/clang_64 NO_DEFAULT_PATH)

# main srcs
set(IMG_HEADERS
  zbbox.h
  zbenchtimer.h
  zbitset.h
  zcpuinfo.h
  zeigenutils.h
  zexception.h
  zglobal.h
  zimage2dutils.h
  zimage3dutils.h
  zimageavx.h
  zimagefilterkernel.h
  zimagesse3.h
  zimg.h
  zimg.hxx
  zimgformat.h
  zimgfreeimage.h
  zimginfo.h
  zimginterface.h
  zimgio.h
  zimgitkimage.h
  zimgjpeg.h
  zimgjpegxr.h
  zimgmetadatabase.h
  zimgmetaimage.h
  zimgmetatag.h
  zimgometiff.h
  zimgpng.h
  zimgregion.h
  zimgsliceprovider.h
  zimgstackinterface.h
  zimgtiff.h
  zimgv3draw.h
  zimgzeissczi.h
  zimgzeisslsm.h
  zioutils.h
  zrandom.h
  zsaturateoperation.h
  zstatisticsutils.h
  zstringutils.h
  ztiff.h
  zvoxelcoordinate.h
  zimgregioniterator.h
  zimgneighborhooditerator.h
  zimgneighborhoodwithcoorditerator.h
  zneighborhood.h
  zimgneighborhoodwithptriterator.h
  zlog.h
  zlogqttypesupport.h
  zwindowsheader.h
  )

set(IMG_SOURCES
  zbenchtimer.cpp
  zcpuinfo.cpp
  zeigenutils.cpp
  zexception.cpp
  zimage2dutils.cpp
  zimage3dutils.cpp
  zimagefilterkernel.cpp
  zimg.cpp
  zimgformat.cpp
  zimgfreeimage.cpp
  zimginfo.cpp
  zimginterface.cpp
  zimgio.cpp
  zimgitkimage.cpp
  zimgjpeg.cpp
  zimgmetaimage.cpp
  zimgmetatag.cpp
  zimgometiff.cpp
  zimgpng.cpp
  zimgregion.cpp
  zimgsliceprovider.cpp
  zimgstackinterface.cpp
  zimgtiff.cpp
  zimgv3draw.cpp
  zimgzeissczi.cpp
  zimgzeisslsm.cpp
  zioutils.cpp
  zrandom.cpp
  zstringutils.cpp
  ztiff.cpp
  zneighborhood.cpp
  zlog.cpp
  zimagesse3.cpp
  zimageavx.cpp
  zimgjpegxr.cpp
  )

# tiff srcs
set(TIFFDIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libtiff/libtiff)
set(TIFF_SOURCES
  ${TIFFDIR}/tif_aux.c
  ${TIFFDIR}/tif_close.c
  ${TIFFDIR}/tif_codec.c
  ${TIFFDIR}/tif_color.c
  ${TIFFDIR}/tif_compress.c
  ${TIFFDIR}/tif_dir.c
  ${TIFFDIR}/tif_dirinfo.c
  ${TIFFDIR}/tif_dirread.c
  ${TIFFDIR}/tif_dirwrite.c
  ${TIFFDIR}/tif_dumpmode.c
  ${TIFFDIR}/tif_error.c
  ${TIFFDIR}/tif_extension.c
  ${TIFFDIR}/tif_fax3.c
  ${TIFFDIR}/tif_fax3sm.c
  ${TIFFDIR}/tif_flush.c
  ${TIFFDIR}/tif_getimage.c
  ${TIFFDIR}/tif_luv.c
  ${TIFFDIR}/tif_lzw.c
  ${TIFFDIR}/tif_next.c
  ${TIFFDIR}/tif_open.c
  ${TIFFDIR}/tif_packbits.c
  ${TIFFDIR}/tif_pixarlog.c
  ${TIFFDIR}/tif_predict.c
  ${TIFFDIR}/tif_print.c
  ${TIFFDIR}/tif_read.c
  ${TIFFDIR}/tif_strip.c
  ${TIFFDIR}/tif_swab.c
  ${TIFFDIR}/tif_thunder.c
  ${TIFFDIR}/tif_tile.c
  ${TIFFDIR}/tif_version.c
  ${TIFFDIR}/tif_warning.c
  ${TIFFDIR}/tif_write.c
  ${TIFFDIR}/tif_zip.c
  ${TIFFDIR}/tif_ojpeg.c
  ${TIFFDIR}/tif_jpeg.c
  ${TIFFDIR}/tif_jbig.c
  ${TIFFDIR}/tif_stream.cxx
  )
if (WIN32)
  set(TIFF_SOURCES ${TIFF_SOURCES} ${TIFFDIR}/tif_win32.c)
else (WIN32)
  set(TIFF_SOURCES ${TIFF_SOURCES} ${TIFFDIR}/tif_unix.c)
endif (WIN32)
source_group(libtiff FILES ${TIFF_SOURCES})

# our target
set(LIBRARY_NAME atlas)
add_library(${LIBRARY_NAME} STATIC ${TIFF_SOURCES} ${IMG_SOURCES} ${IMG_HEADERS})

target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${LIBRARY_NAME} PUBLIC ${TBB_INCLUDE_DIRS})
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/eigen)
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/boost)
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/glm)
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/folly)
target_compile_definitions(${LIBRARY_NAME} PUBLIC EIGEN_NO_DEBUG EIGEN_DONT_PARALLELIZE EIGEN_MPL2_ONLY)
target_compile_definitions(${LIBRARY_NAME} PUBLIC QT_USE_QSTRINGBUILDER QT_NO_CAST_FROM_BYTEARRAY QT_NO_CAST_TO_ASCII)
if (WIN32)
  target_compile_definitions(${LIBRARY_NAME} PRIVATE USE_WIN32_FILEIO) # libtiff
  target_compile_definitions(${LIBRARY_NAME} PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)

  target_compile_options(${LIBRARY_NAME} PUBLIC /GR) # Enable Run-Time Type Information
  target_compile_options(${LIBRARY_NAME} PUBLIC /EHsc) # exception handling model, https://msdn.microsoft.com/en-us/library/1deeycx5.aspx
  target_compile_options(${LIBRARY_NAME} PUBLIC /Wall)
  target_compile_options(${LIBRARY_NAME} PUBLIC /bigobj /GL) # Enables whole program optimization.
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4267) # 'var' : conversion from 'size_t' to 'type', possible loss of data
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4244) # 'argument' : conversion from 'type1' to 'type2', possible loss of data
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4305) # 'identifier' : truncation from 'type1' to 'type2'
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4819) # The file contains a character that cannot be represented in the current code page (number). Save the file in Unicode format to prevent data loss.
  target_compile_options(${LIBRARY_NAME} PUBLIC /utf-8)  # https://blogs.msdn.microsoft.com/vcblog/2016/02/22/new-options-for-managing-character-sets-in-the-microsoft-cc-compiler/

  target_link_libraries(${LIBRARY_NAME} PUBLIC /LTCG) # Link-time Code Generation

  # for some files
  set_source_files_properties(zimageavx.cpp PROPERTIES COMPILE_FLAGS /arch:AVX)

  set_source_files_properties(ztiff.cpp PROPERTIES COMPILE_FLAGS /I${TIFFDIR})
  set_source_files_properties(${TIFFDIR}/tif_ojpeg.c PROPERTIES COMPILE_FLAGS /I${JPEGTURBO_INCLUDE_DIRS})
  set_source_files_properties(${TIFFDIR}/tif_jpeg.c PROPERTIES COMPILE_FLAGS /I${JPEGTURBO_INCLUDE_DIRS})
  set_source_files_properties(zimgjpeg.cpp PROPERTIES COMPILE_FLAGS /I${JPEGTURBO_INCLUDE_DIRS})
  set_source_files_properties(zimgpng.cpp PROPERTIES COMPILE_FLAGS /I${PNG_INCLUDE_DIRS})
  foreach (item IN LISTS JPEGXR_INCLUDE_DIRS)
    message(${item})
    set(JXRINC "${JXRINC} /I${item}")
  endforeach()
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_FLAGS ${JXRINC})
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_DEFINITIONS "__ANSI__;DISABLE_PERF_MEASUREMENT")
  set_source_files_properties(zimgfreeimage.cpp PROPERTIES COMPILE_FLAGS /I${FREEIMAGE_INCLUDE_DIRS})
else (WIN32)
  target_compile_options(${LIBRARY_NAME} PUBLIC -frtti)
  target_compile_options(${LIBRARY_NAME} PUBLIC -fexceptions)
  target_compile_options(${LIBRARY_NAME} PUBLIC -Wall -Wextra)
  target_compile_options(${LIBRARY_NAME} PUBLIC -mllvm -inline-threshold=600 -stdlib=libc++)

  target_link_libraries(${LIBRARY_NAME} PUBLIC -stdlib=libc++)

  # for some files
  set_source_files_properties(zimagesse3.cpp PROPERTIES COMPILE_FLAGS -msse3)
  set_source_files_properties(zimageavx.cpp PROPERTIES COMPILE_FLAGS -mavx)

  set_source_files_properties(ztiff.cpp PROPERTIES COMPILE_FLAGS -I${TIFFDIR})
  set_source_files_properties(${TIFFDIR}/tif_ojpeg.c PROPERTIES COMPILE_FLAGS -I${JPEGTURBO_INCLUDE_DIRS})
  set_source_files_properties(${TIFFDIR}/tif_jpeg.c PROPERTIES COMPILE_FLAGS -I${JPEGTURBO_INCLUDE_DIRS})
  set_source_files_properties(zimgjpeg.cpp PROPERTIES COMPILE_FLAGS -I${JPEGTURBO_INCLUDE_DIRS})
  set_source_files_properties(zimgpng.cpp PROPERTIES COMPILE_FLAGS -I${PNG_INCLUDE_DIRS})
  foreach (item IN LISTS JPEGXR_INCLUDE_DIRS)
    message(${item})
    set(JXRINC "${JXRINC} -I${item}")
  endforeach()
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_FLAGS ${JXRINC})
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_DEFINITIONS "__ANSI__;DISABLE_PERF_MEASUREMENT")
  set_source_files_properties(zimgfreeimage.cpp PROPERTIES COMPILE_FLAGS -I${FREEIMAGE_INCLUDE_DIRS})
endif (WIN32)

target_link_libraries(${LIBRARY_NAME} PUBLIC ${TBB_LIBRARIES} ${ITK_LIBRARIES} ${JPEGTURBO_LIBRARIES} ${PNG_LIBRARIES} ${JPEGXR_LIBRARIES} ${FREEIMAGE_LIBRARIES} glog::glog Qt5::Core)

