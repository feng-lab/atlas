cmake_minimum_required(VERSION 3.6.1)
project(img)

# main stuff

# get_cmake_property(_variableNames VARIABLES)
# foreach (_variableName ${_variableNames})
#   message(STATUS "${_variableName}=${${_variableName}}")
# endforeach ()

include(${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/ceres-solver/ceres-solver.cmake)

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
    zglmutils.h
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
    zimgblockprovider.h
    zimgleica.cpp
    zimgleica.h
    zgmm.h
    zkmeans.h
    zvbgmm.h
    zpuncta.cpp
    zpuncta.h
    zpunctaio.cpp
    zpunctaio.h
    zpunctum.cpp
    zpunctum.h
    zswc.h
    zswc.cpp
    ztree.hpp
    zvoxelregion.h
    zvoxelregion.cpp
    zimgtile.h
    zimgtile.cpp
    zimgitkinterface.h
    zcompleximg.h
    zcompleximg.cpp
    zfft.h
    zfft.cpp
    zaffine2d.h
    zaffine3d.h
    zaffine2d.cpp
    zaffine3d.cpp
    zimageinterpolation.h
    zimageinterpolation.cpp
    zimagetoimagemetric.h
    zimagetoimagemetric.cpp
    zimagetransform.h
    zimagetransform.cpp
    zimagematrix2dtransform.h
    zimagematrix2dtransform.cpp
    zimagematrix3dtransform.h
    zimagematrix3dtransform.cpp
    zimagecompositetransform.h
    zimagecompositetransform.cpp
    zimagetransformresolve.h
    zimagetransformresolve.cpp
    zimgmerge.h
    zimgncc.h
    zimgmerge.cpp
    zimgncc.cpp
    zimgalgorithm.h
    zimgconnectedcomponents.h
    zimgregionalextrema.h
    zimgautothreshold.h
    zimgalgorithm.cpp
    zimgconnectedcomponents.cpp
    zimgregionalextrema.cpp
    zimgautothreshold.cpp
    zimgsigneddistancemap.h
    zimgsigneddistancemap.cpp
    zimggraph.h
    zimgnccmatch.h
    zimggraph.cpp
    zimgnccmatch.cpp
    zimgfillhole.h
    zimgfillhole.cpp
    zimgregistration.h
    zimgregistration.cpp
    zregistrationoptimizer.h
    zregistrationcostfunction.h
    zregistrationoptimizer.cpp
    zregistrationcostfunction.cpp
    zregistrationnumericdiffcostfunction.h
    zregistrationnumericdiffcostfunction.cpp
    zimgprocess.h
    zimgprocess.cpp
    zassignpuncta.h
    zassignpuncta.cpp
    zchromaticshiftcorrection.cpp
    zchromaticshiftcorrection.h
    zpunctadetection.h
    zpunctadetection.cpp
    zsectionsregistration.h
    zsectionsregistration.cpp
    zstitchimage.cpp
    zstitchimage.h
    )

# our target
set(LIBRARY_NAME img)
add_library(${LIBRARY_NAME} STATIC ${TIFF_SOURCES} ${IMG_SOURCES}
            ${CERESSOLVER_HEADERS} ${CERESSOLVER_INTERNAL}
            )

target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${LIBRARY_NAME} PUBLIC ${MKL_INCLUDE_DIRS})
target_include_directories(${LIBRARY_NAME} PUBLIC ${CERESSOLVER_INCLUDE_DIRS})
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/eigen)
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/boost)
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/glm)
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/folly)
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/scopeguard)
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/better-enums)
target_compile_definitions(${LIBRARY_NAME} PUBLIC EIGEN_NO_DEBUG EIGEN_DONT_PARALLELIZE EIGEN_MPL2_ONLY)
target_compile_definitions(${LIBRARY_NAME} PUBLIC EIGEN_USE_BLAS EIGEN_USE_LAPACKE_STRICT) # EIGEN_USE_MKL_VML)
target_compile_definitions(${LIBRARY_NAME} PUBLIC lapack_complex_float=std::complex<float> lapack_complex_double=std::complex<double>)
target_compile_definitions(${LIBRARY_NAME} PUBLIC QT_USE_QSTRINGBUILDER QT_NO_CAST_FROM_BYTEARRAY QT_NO_CAST_TO_ASCII QT_NO_VERSION_TAGGING)
if (WIN32)
  target_compile_definitions(${LIBRARY_NAME} PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0601)

  target_compile_options(${LIBRARY_NAME} PUBLIC /GR) # Enable Run-Time Type Information
  target_compile_options(${LIBRARY_NAME} PUBLIC /EHsc) # exception handling model, https://msdn.microsoft.com/en-us/library/1deeycx5.aspx
  # target_compile_options(${LIBRARY_NAME} PUBLIC /Wall)
  target_compile_options(${LIBRARY_NAME} PUBLIC /bigobj) # increases the number of sections that an object file can contain.
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
  set_source_files_properties(zcpuinfo.cpp PROPERTIES COMPILE_FLAGS /I${CPUINFO_INCLUDE_DIRS})
  foreach (item IN LISTS JPEGXR_INCLUDE_DIRS)
    message(STATUS "JPEGXR_INCLUDE_DIRS: ${item}")
    set(JXRINC "${JXRINC} /I${item}")
  endforeach ()
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_FLAGS ${JXRINC})
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_DEFINITIONS "__ANSI__;DISABLE_PERF_MEASUREMENT")
  set_source_files_properties(zimgfreeimage.cpp PROPERTIES COMPILE_FLAGS /I${FREEIMAGE_INCLUDE_DIRS})

  set_source_files_properties(${CERESSOLVER_INTERNAL} PROPERTIES COMPILE_FLAGS
                              "/wd4100 /I${CERESSOLVER_INTERNAL_INCLUDE_DIRS}") # 'identifier' : unreferenced formal parameter)
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
  set_source_files_properties(zcpuinfo.cpp PROPERTIES COMPILE_FLAGS -I${CPUINFO_INCLUDE_DIRS})
  foreach (item IN LISTS JPEGXR_INCLUDE_DIRS)
    message(STATUS "JPEGXR_INCLUDE_DIRS: ${item}")
    set(JXRINC "${JXRINC} -I${item}")
  endforeach ()
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_FLAGS ${JXRINC})
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_DEFINITIONS "__ANSI__;DISABLE_PERF_MEASUREMENT")
  set_source_files_properties(zimgfreeimage.cpp PROPERTIES COMPILE_FLAGS -I${FREEIMAGE_INCLUDE_DIRS})

  set_source_files_properties(zregistrationoptimizer.cpp PROPERTIES COMPILE_FLAGS -Wno-shadow)

  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(${CERESSOLVER_INTERNAL} PROPERTIES COMPILE_FLAGS
                                "-Wno-unused-parameter -Wno-sign-compare -Wno-shadow -Wno-ignored-qualifiers \
                              -Wno-unknown-pragmas -Wno-unused-variable -Wno-reorder \
                              -I${CERESSOLVER_INTERNAL_INCLUDE_DIRS}")
  elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(${CERESSOLVER_INTERNAL} PROPERTIES COMPILE_FLAGS
                                "-Wno-unused-parameter -Wno-sign-compare -Wno-shadow -Wno-ignored-qualifiers \
                              -Wno-unknown-pragmas -Wno-unused-variable -Wno-reorder -Wno-maybe-uninitialized \
                              -I${CERESSOLVER_INTERNAL_INCLUDE_DIRS}")
  endif ()
endif (WIN32)

target_link_libraries(${LIBRARY_NAME} PUBLIC TBB::tbb ${ITK_LIBRARIES} ${JPEGTURBO_LIBRARIES}
                      png_static ${JPEGXR_LIBRARIES} ${FREEIMAGE_LIBRARIES} ${CPUINFO_LIBRARIES}
                      hdf5::hdf5_cpp-static hdf5::hdf5-static
                      glog::glog Qt5::Core)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_link_libraries(${LIBRARY_NAME} PUBLIC -Wl,--start-group ${MKL_LIBRARIES} -Wl,--end-group)
else ()
  target_link_libraries(${LIBRARY_NAME} PUBLIC ${MKL_LIBRARIES})
endif ()
