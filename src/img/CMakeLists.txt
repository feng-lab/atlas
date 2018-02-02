cmake_minimum_required(VERSION 3.6.1)
project(img)

LIST(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# main stuff

find_package(TBB REQUIRED)
message(STATUS "TBB_LIBRARY: ${TBB_LIBRARY}")

find_package(ITK REQUIRED COMPONENTS ITKIOMeta ITKIONIFTI ITKIONRRD ITKIOGDCM ITKBinaryMathematicalMorphology
             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/itk NO_DEFAULT_PATH)
message(STATUS "ITK_DIR: ${ITK_DIR}")
message(STATUS "ITK_USE_FILE: ${ITK_USE_FILE}")
include(${ITK_USE_FILE})
message(STATUS "ITK_INCLUDE_DIRS: ${ITK_INCLUDE_DIRS}")
message(STATUS "ITK_LIBRARIES: ${ITK_LIBRARIES}")

#find_package(libjpeg REQUIRED PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libjpeg-turbo NO_DEFAULT_PATH)
set(JPEGTURBO_INCLUDE_DIRS ${JPEGTURBO_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libjpeg-turbo/include)
if (WIN32)
  set(JPEGTURBO_LIBRARIES ${JPEGTURBO_LIBRARIES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libjpeg-turbo/lib/jpeg-static.lib)
else (WIN32)
  set(JPEGTURBO_LIBRARIES ${JPEGTURBO_LIBRARIES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libjpeg-turbo/lib/libjpeg.a)
endif (WIN32)

# libpng16.cmake does not provide include dir, so we have to create it, note PNG_INCLUDE_DIRS will only used by 1 file
include(${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libpng/lib/libpng/libpng16.cmake)
set(PNG_INCLUDE_DIRS ${PNG_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libpng/include)
#if (WIN32)
#  set(PNG_LIBRARIES ${PNG_LIBRARIES}
#      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libpng/lib/png-static.lib)
#else (WIN32)
#  set(PNG_LIBRARIES ${PNG_LIBRARIES}
#      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libpng/lib/libpng.a)
#endif (WIN32)
#find_package(libpng16 REQUIRED PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libpng/lib/libpng NO_DEFAULT_PATH)

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

if (WIN32)
  set(FREEIMAGE_INCLUDE_DIRS ${FREEIMAGE_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/freeimage)
  set(FREEIMAGE_LIBRARIES ${FREEIMAGE_LIBRARIES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/freeimage/FreeImagePlus.lib
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/freeimage/FreeImage.lib)
elseif (APPLE)
  set(FREEIMAGE_INCLUDE_DIRS ${FREEIMAGE_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/freeimage/include)
  set(FREEIMAGE_LIBRARIES ${FREEIMAGE_LIBRARIES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/freeimage/lib/libfreeimageplus.dylib)
else ()
  set(FREEIMAGE_INCLUDE_DIRS ${FREEIMAGE_INCLUDE_DIRS}
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/freeimage/include)
  set(FREEIMAGE_LIBRARIES ${FREEIMAGE_LIBRARIES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/freeimage/lib/libfreeimageplus.so)
endif ()

find_package(gflags REQUIRED
             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/gflags NO_DEFAULT_PATH)
find_package(glog REQUIRED
             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/glog ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/gflags
             NO_DEFAULT_PATH)

find_package(HDF5 REQUIRED COMPONENTS C CXX static
             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/hdf5/share ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/hdf5/cmake NO_DEFAULT_PATH)
message(STATUS "HDF5_INCLUDE_DIR: ${HDF5_INCLUDE_DIR}")

find_package(Qt5Core ${QT_VERSION} REQUIRED PATHS ${QT_PATHS})

# get_cmake_property(_variableNames VARIABLES)
# foreach (_variableName ${_variableNames})
#   message(STATUS "${_variableName}=${${_variableName}}")
# endforeach ()

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
  set(TIFF_SOURCES ${TIFF_SOURCES} ${TIFFDIR}/tif_unix.c)
else (WIN32)
  set(TIFF_SOURCES ${TIFF_SOURCES} ${TIFFDIR}/tif_unix.c)
endif (WIN32)
source_group(libtiff FILES ${TIFF_SOURCES})

# main srcs
set(IMG_SOURCES
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
    zimg.inl
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
    zbenchtimer.cpp
    zcpuinfo.cpp
    zeigenutils.cpp
    zimage2dutils.cpp
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
    zexception.cpp
    zflags.h
    zimghdf5.cpp
    zimghdf5.h
    zimginfoio.cpp
    zimginfoio.h
    zimgblockprovider.h zimgleica.cpp zimgleica.h)

# our target
set(LIBRARY_NAME img)
add_library(${LIBRARY_NAME} STATIC ${TIFF_SOURCES} ${IMG_SOURCES})

target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/eigen)
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/boost)
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/glm)
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/folly)
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/scopeguard)
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${TBB_INCLUDE_DIR})
target_include_directories(${LIBRARY_NAME} PUBLIC ${HDF5_INCLUDE_DIR})
target_compile_definitions(${LIBRARY_NAME} PUBLIC EIGEN_NO_DEBUG EIGEN_DONT_PARALLELIZE EIGEN_MPL2_ONLY)
target_compile_definitions(${LIBRARY_NAME} PUBLIC QT_USE_QSTRINGBUILDER QT_NO_CAST_FROM_BYTEARRAY QT_NO_CAST_TO_ASCII)
if (WIN32)
  target_compile_definitions(${LIBRARY_NAME} PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

  target_compile_options(${LIBRARY_NAME} PUBLIC /GR) # Enable Run-Time Type Information
  target_compile_options(${LIBRARY_NAME} PUBLIC /EHsc) # exception handling model, https://msdn.microsoft.com/en-us/library/1deeycx5.aspx
  # target_compile_options(${LIBRARY_NAME} PUBLIC /Wall)
  target_compile_options(${LIBRARY_NAME} PUBLIC /bigobj /GL) # Enables whole program optimization.
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4267) # 'var' : conversion from 'size_t' to 'type', possible loss of data
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4244) # 'argument' : conversion from 'type1' to 'type2', possible loss of data
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4305) # 'identifier' : truncation from 'type1' to 'type2'
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4819) # The file contains a character that cannot be represented in the current code page (number). Save the file in Unicode format to prevent data loss.
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4828) # The file contains a character starting at offset 0x60 that is illegal in the current source character set (codepage 65001).
  target_compile_options(${LIBRARY_NAME} PUBLIC /utf-8)  # https://blogs.msdn.microsoft.com/vcblog/2016/02/22/new-options-for-managing-character-sets-in-the-microsoft-cc-compiler/
  target_compile_options(${LIBRARY_NAME} PUBLIC /analyze) # The C/C++ Code Analysis tool provides information to developers about possible defects in their C/C++ source code
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd6255) # _alloca indicates failure by raising a stack overflow exception. Consider using _malloca instead.
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd6326) # Potential comparison of a constant with another constant.
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd6330) # Incorrect type passed as parameter in call to function.
  target_compile_options(${LIBRARY_NAME} PUBLIC /MP) # Build with Multiple Processes
  target_compile_options(${LIBRARY_NAME} PUBLIC /Zc:inline) #
  target_compile_options(${LIBRARY_NAME} PUBLIC /GL) # Enables whole program optimization.
  target_compile_options(${LIBRARY_NAME} PUBLIC /permissive-)

  #target_link_libraries(${LIBRARY_NAME} PUBLIC /LTCG) # Link-time Code Generation
  set_target_properties(${LIBRARY_NAME} PROPERTIES LINK_FLAGS /LTCG)

  # for some files
  set_source_files_properties(zimageavx.cpp PROPERTIES COMPILE_FLAGS /arch:AVX)

  set_source_files_properties(ztiff.cpp PROPERTIES COMPILE_FLAGS /I${TIFFDIR})
  set_source_files_properties(${TIFFDIR}/tif_ojpeg.c PROPERTIES COMPILE_FLAGS /I${JPEGTURBO_INCLUDE_DIRS})
  set_source_files_properties(${TIFFDIR}/tif_jpeg.c PROPERTIES COMPILE_FLAGS /I${JPEGTURBO_INCLUDE_DIRS})
  set_source_files_properties(zimgjpeg.cpp PROPERTIES COMPILE_FLAGS /I${JPEGTURBO_INCLUDE_DIRS})
  set_source_files_properties(zimgpng.cpp PROPERTIES COMPILE_FLAGS /I${PNG_INCLUDE_DIRS})
  foreach (item IN LISTS JPEGXR_INCLUDE_DIRS)
    message(STATUS "JPEGXR_INCLUDE_DIRS: ${item}")
    set(JXRINC "${JXRINC} /I${item}")
  endforeach ()
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_FLAGS ${JXRINC})
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_DEFINITIONS "__ANSI__;DISABLE_PERF_MEASUREMENT")
  set_source_files_properties(zimgfreeimage.cpp PROPERTIES COMPILE_FLAGS /I${FREEIMAGE_INCLUDE_DIRS})
else (WIN32)
  target_compile_options(${LIBRARY_NAME} PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-frtti>)
  target_compile_options(${LIBRARY_NAME} PUBLIC -fexceptions)
  target_compile_options(${LIBRARY_NAME} PUBLIC -Wall -Wextra)
  target_compile_options(${LIBRARY_NAME} PUBLIC -Waddress -Wdeprecated)

  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${LIBRARY_NAME} PUBLIC -Wbool-conversion -Wconstant-conversion -Wenum-conversion)
    target_compile_options(${LIBRARY_NAME} PUBLIC -Wc++14-compat-pedantic -Waddress-of-temporary)
    target_compile_options(${LIBRARY_NAME} PRIVATE -Wundefined-reinterpret-cast)
    target_compile_options(${LIBRARY_NAME} PUBLIC -mllvm -inline-threshold=600 -stdlib=libc++)
    target_compile_options(${LIBRARY_NAME} PUBLIC -Wshadow)
    #target_link_libraries(${LIBRARY_NAME} PUBLIC -stdlib=libc++)
    set_target_properties(${LIBRARY_NAME} PROPERTIES LINK_FLAGS -stdlib=libc++)
    set_source_files_properties(${TIFF_SOURCES} PROPERTIES COMPILE_FLAGS -Wno-unused-parameter)
  elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(${TIFF_SOURCES} PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -Wno-unused-but-set-variable")
  endif ()

  # for some files
  set_source_files_properties(zimageavx.cpp PROPERTIES COMPILE_FLAGS -mavx)
  set_source_files_properties(zimagesse3.cpp PROPERTIES COMPILE_FLAGS -msse3)

  set_source_files_properties(ztiff.cpp PROPERTIES COMPILE_FLAGS -I${TIFFDIR})
  set_source_files_properties(${TIFFDIR}/tif_ojpeg.c
                              PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -I${JPEGTURBO_INCLUDE_DIRS}")
  set_source_files_properties(${TIFFDIR}/tif_jpeg.c
                              PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -I${JPEGTURBO_INCLUDE_DIRS}")
  set_source_files_properties(zimgjpeg.cpp PROPERTIES COMPILE_FLAGS -I${JPEGTURBO_INCLUDE_DIRS})
  set_source_files_properties(zimgpng.cpp PROPERTIES COMPILE_FLAGS -I${PNG_INCLUDE_DIRS})
  foreach (item IN LISTS JPEGXR_INCLUDE_DIRS)
    message(STATUS "JPEGXR_INCLUDE_DIRS: ${item}")
    set(JXRINC "${JXRINC} -I${item}")
  endforeach ()
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_FLAGS ${JXRINC})
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_DEFINITIONS "__ANSI__;DISABLE_PERF_MEASUREMENT")
  set_source_files_properties(zimgfreeimage.cpp PROPERTIES COMPILE_FLAGS -I${FREEIMAGE_INCLUDE_DIRS})
endif (WIN32)

target_link_libraries(${LIBRARY_NAME} PUBLIC ${TBB_LIBRARY} ${ITK_LIBRARIES} ${JPEGTURBO_LIBRARIES}
                      png_static ${JPEGXR_LIBRARIES} ${FREEIMAGE_LIBRARIES} hdf5::hdf5_cpp-static hdf5::hdf5-static
                      glog::glog Qt5::Core)


# mex file
find_package(Matlab COMPONENTS MX_LIBRARY MEX_COMPILER)
if (Matlab_FOUND)
  message(STATUS "Matlab_ROOT_DIR: ${Matlab_ROOT_DIR}")
  message(STATUS "Matlab_INCLUDE_DIRS: ${Matlab_INCLUDE_DIRS}")
  message(STATUS "Matlab_MEX_LIBRARY: ${Matlab_MEX_LIBRARY}")
  message(STATUS "Matlab_MX_LIBRARY: ${Matlab_MX_LIBRARY}")
  message(STATUS "Matlab_LIBRARIES: ${Matlab_LIBRARIES}")
  message(STATUS "Matlab_MEX_COMPILER: ${Matlab_MEX_COMPILER}")

  if (APPLE)
    function(add_mex_target mexname)
      matlab_add_mex(
        NAME ${mexname}
        SRC ${CMAKE_CURRENT_SOURCE_DIR}/matlab/${mexname}.cpp
        LINK_TO ${LIBRARY_NAME}
      )
      add_custom_command(TARGET ${mexname}
                         POST_BUILD
                         COMMAND install_name_tool -change
                         @rpath/QtCore.framework/Versions/5/QtCore
                         @loader_path/QtCore
                         $<TARGET_FILE:${mexname}>
                         COMMAND install_name_tool -change
                         /Users/feng/code/atlas/src/3rdparty/freeimage/lib/libfreeimageplus.dylib
                         @loader_path/libfreeimageplus.dylib
                         $<TARGET_FILE:${mexname}>
                         COMMAND install_name_tool -change
                         @rpath/libtbb.dylib
                         @loader_path/libtbb.dylib
                         $<TARGET_FILE:${mexname}>
                         )
    endfunction(add_mex_target)

    add_mex_target(correct_czi_shading_mex)
    add_mex_target(read_stack_info_mex)
    add_mex_target(read_stack_mex)
    add_mex_target(stack_czi_tiles_mex)
    add_mex_target(stack_czi_tiles_with_mask_mex)

    add_custom_command(TARGET correct_czi_shading_mex
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy ${TBB_LIBRARY}
                       $<TARGET_FILE_DIR:correct_czi_shading_mex>
                       COMMAND ${CMAKE_COMMAND} -E copy ${FREEIMAGE_LIBRARIES}
                       $<TARGET_FILE_DIR:correct_czi_shading_mex>
                       COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:Qt5::Core>
                       $<TARGET_FILE_DIR:correct_czi_shading_mex>
                       COMMAND install_name_tool -id QtCore
                       $<TARGET_FILE_DIR:correct_czi_shading_mex>/QtCore
                       COMMAND install_name_tool -id libfreeimageplus.dylib
                       $<TARGET_FILE_DIR:correct_czi_shading_mex>/libfreeimageplus.dylib
                       COMMAND install_name_tool -id libtbb.dylib
                       $<TARGET_FILE_DIR:correct_czi_shading_mex>/libtbb.dylib
                       )
  endif (APPLE)
endif (Matlab_FOUND)



