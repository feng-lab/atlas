cmake_minimum_required(VERSION 3.5.2)
project(img)

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

find_package(TBB REQUIRED)
message(${TBB_INCLUDE_DIRS})
message("${TBB_tbb_LIBRARY_RELEASE}")

find_package(ITK REQUIRED COMPONENTS ITKIOMeta ITKIONIFTI ITKIONRRD ITKIOGDCM ITKBinaryMathematicalMorphology
             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/itk NO_DEFAULT_PATH)
message(${ITK_DIR})
message(${ITK_USE_FILE})
include(${ITK_USE_FILE})
message("${ITK_INCLUDE_DIRS}")
message("${ITK_LIBRARIES}")

#find_package(JPEG REQUIRED PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty NO_DEFAULT_PATH)
set(JPEGTURBO_INCLUDE_DIRS ${JPEGTURBO_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libjpeg-turbo/include)
if (WIN32)
  set(JPEGTURBO_LIBRARIES ${JPEGTURBO_LIBRARIES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libjpeg-turbo/lib/jpeg-static.lib)
else (WIN32)
  set(JPEGTURBO_LIBRARIES ${JPEGTURBO_LIBRARIES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libjpeg-turbo/lib/libjpeg.a)
endif (WIN32)

#find_package(libpng16 REQUIRED PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libpng NO_DEFAULT_PATH)
set(PNG_INCLUDE_DIRS ${PNG_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libpng/include)
if (WIN32)
  set(PNG_LIBRARIES ${PNG_LIBRARIES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libpng/lib/png-static.lib)
else (WIN32)
  set(PNG_LIBRARIES ${PNG_LIBRARIES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libpng/lib/libpng.a)
endif (WIN32)

set(JPEGXR_INCLUDE_DIRS ${JPEGXR_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/jxrlib/include/libjxr/common
    ${JPEGXR_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/jxrlib/include/libjxr/image/x86
    ${JPEGXR_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/jxrlib/include/libjxr/image
    ${JPEGXR_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/jxrlib/include/libjxr/glue)
if (WIN32)
  set(JPEGXR_LIBRARIES ${JPEGXR_LIBRARIES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/jxrlib/lib/JXRGlueLib.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/jxrlib/lib/JXRDecodeLib.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/jxrlib/lib/JXREncodeLib.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/jxrlib/lib/JXRCommonLib.lib)
else (WIN32)
  set(JPEGXR_LIBRARIES ${JPEGXR_LIBRARIES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/jxrlib/lib/libjxrglue.a
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/jxrlib/lib/libjpegxr.a)
endif (WIN32)

set(FREEIMAGE_INCLUDE_DIRS ${FREEIMAGE_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/freeimage/include)
if (WIN32)
  set(FREEIMAGE_LIBRARIES ${FREEIMAGE_LIBRARIES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/freeimage/lib/FreeImagePlus.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/freeimage/lib/FreeImage.lib)
else (WIN32)
  set(FREEIMAGE_LIBRARIES ${FREEIMAGE_LIBRARIES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/freeimage/lib/libfreeimageplus.dylib)
endif (WIN32)

find_package(gflags REQUIRED PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/gflags NO_DEFAULT_PATH)
message(${GFLAGS_INCLUDE_DIR})
message(${GFLAGS_LIBRARIES})

find_package(glog REQUIRED
             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/glog ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/gflags
             NO_DEFAULT_PATH)

find_package(Qt5Core REQUIRED PATHS /Users/feng/Qt/5.7/clang_64 NO_DEFAULT_PATH)

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
set(LIBRARY_NAME img)
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
  endforeach ()
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

  set_source_files_properties(${TIFF_SOURCES} PROPERTIES COMPILE_FLAGS -Wno-unused-parameter)
  set_source_files_properties(ztiff.cpp PROPERTIES COMPILE_FLAGS -I${TIFFDIR})
  set_source_files_properties(${TIFFDIR}/tif_ojpeg.c
                              PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -I${JPEGTURBO_INCLUDE_DIRS}")
  set_source_files_properties(${TIFFDIR}/tif_jpeg.c
                              PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -I${JPEGTURBO_INCLUDE_DIRS}")
  set_source_files_properties(zimgjpeg.cpp PROPERTIES COMPILE_FLAGS -I${JPEGTURBO_INCLUDE_DIRS})
  set_source_files_properties(zimgpng.cpp PROPERTIES COMPILE_FLAGS -I${PNG_INCLUDE_DIRS})
  foreach (item IN LISTS JPEGXR_INCLUDE_DIRS)
    message(${item})
    set(JXRINC "${JXRINC} -I${item}")
  endforeach ()
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_FLAGS ${JXRINC})
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_DEFINITIONS "__ANSI__;DISABLE_PERF_MEASUREMENT")
  set_source_files_properties(zimgfreeimage.cpp PROPERTIES COMPILE_FLAGS -I${FREEIMAGE_INCLUDE_DIRS})
endif (WIN32)

target_link_libraries(${LIBRARY_NAME} PUBLIC ${TBB_tbb_LIBRARY_RELEASE} ${ITK_LIBRARIES} ${JPEGTURBO_LIBRARIES}
                      ${PNG_LIBRARIES} ${JPEGXR_LIBRARIES} ${FREEIMAGE_LIBRARIES} glog::glog Qt5::Core)

