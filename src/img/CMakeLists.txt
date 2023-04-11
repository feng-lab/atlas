cmake_minimum_required(VERSION 3.17.5)
project(img)

# main stuff

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
    ${TIFFDIR}/tif_jpeg.c
    ${TIFFDIR}/tif_lzma.c
    ${TIFFDIR}/tif_zstd.c
    ${TIFFDIR}/tif_webp.c
    ${TIFFDIR}/tif_stream.cxx
    )
if (WIN32)
  set(TIFF_SOURCES ${TIFF_SOURCES} ${TIFFDIR}/tif_unix.c)
else (WIN32)
  set(TIFF_SOURCES ${TIFF_SOURCES} ${TIFFDIR}/tif_unix.c)
endif (WIN32)
source_group(libtiff FILES ${TIFF_SOURCES})

# tiff srcs
set(SCIFIODIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/scifio)
set(SCIFIO_SOURCES
    ${SCIFIODIR}/itkSCIFIOImageIO.cxx
    ${SCIFIODIR}/itkSCIFIOImageIOFactory.cxx
    )
source_group(scifio FILES ${SCIFIO_SOURCES})

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
    # zimgstackinterface.h
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
    # zimgstackinterface.cpp
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
    zimginit.h
    zimginit.cpp
    zjson.h
    zjson.cpp
    zroiutils.cpp
    zroiutils.h
    zlogcache.cpp
    zlogcache.h
    zminimumspanningtree.cpp
    zminimumspanningtree.h zsubrange.h zmkl.h zsaturateoperation.cpp
    zsaturateoperation_avx2.cpp zsaturateoperation_avx512.cpp zsaturateoperation_avx2.h zsaturateoperation_avx512.h
    zimageavx512.h zimageavx512.cpp
    zmesh.h zmesh.cpp
    zmeshio.h zmeshio.cpp
    zmeshutils.h zmeshutils.cpp
    zimginterpolate.h zimginterpolate.cpp
    zimgblockprovider.cpp
    zmemorymappedfile.cpp zmemorymappedfile.h
    zmemorymappedfilecache.cpp zmemorymappedfilecache.h
    zh5zjpegxr.cpp zh5zjpegxr.h zhashutils.h zfolly.cpp zfolly.h)

message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")

# our target
if (BUILD_WITH_CONDA)
  set(LIBRARY_NAME condaimg)
else ()
  set(LIBRARY_NAME img)
endif ()
add_library(${LIBRARY_NAME} STATIC ${TIFF_SOURCES} ${SCIFIO_SOURCES} ${IMG_SOURCES}
            )

if (BUILD_WITH_CONDA)
  set(IPP_LIBRARIES "")
else ()
  target_compile_definitions(${LIBRARY_NAME} PUBLIC ZIMG_USE_IPP)
  set(IPP_LIBRARIES IPP::ippi)
endif ()
target_compile_definitions(${LIBRARY_NAME} PUBLIC ZIMG_USE_MKL)

if (WIN32)
  target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/include)
endif ()
target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if (NOT WIN32)
  target_include_directories(${LIBRARY_NAME} SYSTEM BEFORE PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/include)
endif ()
target_include_directories(${LIBRARY_NAME} SYSTEM PRIVATE ${MKL_INCLUDE_DIRS})
target_include_directories(${LIBRARY_NAME} SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/simde)
target_compile_definitions(${LIBRARY_NAME} PUBLIC EIGEN_NO_DEBUG EIGEN_DONT_PARALLELIZE)
target_compile_definitions(${LIBRARY_NAME} PUBLIC EIGEN_USE_BLAS EIGEN_USE_LAPACKE_STRICT) # EIGEN_USE_MKL_VML)
target_compile_definitions(${LIBRARY_NAME} PUBLIC lapack_complex_float=std::complex<float> lapack_complex_double=std::complex<double>)
target_compile_definitions(${LIBRARY_NAME} PUBLIC QT_NO_KEYWORDS QT_USE_QSTRINGBUILDER QT_NO_CAST_FROM_BYTEARRAY QT_NO_CAST_TO_ASCII QT_NO_VERSION_TAGGING)
target_compile_definitions(${LIBRARY_NAME} PUBLIC BOOST_CONTAINER_NO_LIB BOOST_JSON_NO_LIB)
if (WIN32)
  target_compile_definitions(${LIBRARY_NAME} PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE WINVER=0x0602 _WIN32_WINNT=0x0602 _USE_MATH_DEFINES _SILENCE_CXX17_RESULT_OF_DEPRECATION_WARNING _SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING _HAS_DEPRECATED_RESULT_OF)

  target_compile_options(${LIBRARY_NAME} PUBLIC /GR) # Enable Run-Time Type Information
  target_compile_options(${LIBRARY_NAME} PUBLIC /EHsc) # exception handling model, https://msdn.microsoft.com/en-us/library/1deeycx5.aspx
  # target_compile_options(${LIBRARY_NAME} PUBLIC /Wall)
  target_compile_options(${LIBRARY_NAME} PUBLIC /bigobj) # increases the number of sections that an object file can contain.
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4267) # 'var' : conversion from 'size_t' to 'type', possible loss of data
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4244) # 'argument' : conversion from 'type1' to 'type2', possible loss of data
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4305) # 'identifier' : truncation from 'type1' to 'type2'
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4819) # The file contains a character that cannot be represented in the current code page (number). Save the file in Unicode format to prevent data loss.
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd4828) # The file contains a character starting at offset 0x60 that is illegal in the current source character set (codepage 65001).
  target_compile_options(${LIBRARY_NAME} PUBLIC /wd5105) # macro expansion producing 'defined' has undefined behavior, from windows sdk
  target_compile_options(${LIBRARY_NAME} PUBLIC /utf-8)  # https://blogs.msdn.microsoft.com/vcblog/2016/02/22/new-options-for-managing-character-sets-in-the-microsoft-cc-compiler/
  if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${LIBRARY_NAME} PUBLIC /analyze) # The C/C++ Code Analysis tool provides information to developers about possible defects in their C/C++ source code
    # target_compile_options(${LIBRARY_NAME} PUBLIC /wd6255) # _alloca indicates failure by raising a stack overflow exception. Consider using _malloca instead.
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd6326) # Potential comparison of a constant with another constant.
    # target_compile_options(${LIBRARY_NAME} PUBLIC /wd6330) # Incorrect type passed as parameter in call to function.
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd6386) # Buffer overrun while writing to 'data'.
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd28020)
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd6239) #  (<non-zero constant> && <expression>) always evaluates to the result of <expression>. Did you intend to use the bitwise-and operator?
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd6336) # Arithmetic operator has precedence over question operator, use parentheses to clarify intent.
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd6255) # _alloca indicates failure by raising a stack overflow exception.  Consider using _malloca instead.
    target_compile_options(${LIBRARY_NAME} PUBLIC /MP) # Build with Multiple Processes
    target_compile_options(${LIBRARY_NAME} PUBLIC /GL) # Enables whole program optimization.
  else ()
    target_compile_options(${LIBRARY_NAME} PUBLIC /MD)
  endif ()
  target_compile_options(${LIBRARY_NAME} PUBLIC /Zc:inline) #
  target_compile_options(${LIBRARY_NAME} PUBLIC /permissive-)
  target_compile_options(${LIBRARY_NAME} PUBLIC /Zc:preprocessor)
  target_compile_options(${LIBRARY_NAME} PUBLIC /arch:AVX)
  # target_compile_options(${LIBRARY_NAME} PUBLIC /Zi)
  target_compile_options(${LIBRARY_NAME} PUBLIC /Zc:__cplusplus) # The /Zc:__cplusplus compiler option enables the __cplusplus preprocessor macro to report an updated value for recent C++ language standards support

  target_link_options(${LIBRARY_NAME} PUBLIC /LTCG:STATUS)

  # for some files
  set_source_files_properties(zimageavx512.cpp PROPERTIES COMPILE_FLAGS /arch:AVX512)
  set_source_files_properties(zsaturateoperation_avx512.cpp PROPERTIES COMPILE_FLAGS /arch:AVX512)
  set_source_files_properties(zsaturateoperation_avx2.cpp PROPERTIES COMPILE_FLAGS /arch:AVX2)
  set_source_files_properties(zimageavx.cpp PROPERTIES COMPILE_FLAGS /arch:AVX)
  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(zimagesse3.cpp PROPERTIES COMPILE_FLAGS -msse3)
  endif ()

  set_source_files_properties(ztiff.cpp PROPERTIES COMPILE_FLAGS /I${TIFFDIR})
  set_source_files_properties(zimgpng.cpp PROPERTIES COMPILE_FLAGS /I${PNG_INCLUDE_DIRS})
  set_source_files_properties(zimgfreeimage.cpp PROPERTIES COMPILE_FLAGS /I${FREEIMAGE_INCLUDE_DIRS})
  foreach (item IN LISTS JPEGXR_INCLUDE_DIRS)
    message(STATUS "JPEGXR_INCLUDE_DIRS: ${item}")
    set(JXRINC "${JXRINC} /I${item}")
  endforeach ()
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_FLAGS ${JXRINC})
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_DEFINITIONS "WIN32;DISABLE_PERF_MEASUREMENT")
  set_source_files_properties(zimgitkimage.cpp PROPERTIES COMPILE_FLAGS /I${SCIFIODIR})
  set_source_files_properties(zimginterface.cpp PROPERTIES COMPILE_FLAGS /I${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/magic_enum/include)
  # set_source_files_properties(zroiutils2.cpp PROPERTIES COMPILE_FLAGS /I${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/include/skia)
  #set_source_files_properties(zimggraph.cpp PROPERTIES COMPILE_FLAGS /Zc:preprocessor-)
  #set_source_files_properties(zimagetransformresolve.cpp PROPERTIES COMPILE_FLAGS /Zc:preprocessor-)
  #set_source_files_properties(zminimumspanningtree.cpp PROPERTIES COMPILE_FLAGS /Zc:preprocessor-)
else (WIN32)
  target_compile_options(${LIBRARY_NAME} PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-frtti>)
  target_compile_options(${LIBRARY_NAME} PUBLIC -fexceptions -fwrapv -fno-strict-aliasing)
  target_compile_options(${LIBRARY_NAME} PUBLIC -Wall -Wextra)
  target_compile_options(${LIBRARY_NAME} PUBLIC -Waddress -Wdeprecated)
  target_compile_options(${LIBRARY_NAME} PUBLIC -mavx -fopenmp-simd -DSIMDE_ENABLE_OPENMP)
  target_compile_options(${LIBRARY_NAME} PUBLIC -fno-common)

  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${LIBRARY_NAME} PUBLIC -Wbool-conversion -Wconstant-conversion -Wenum-conversion)
    target_compile_options(${LIBRARY_NAME} PUBLIC -Waddress-of-temporary)
    target_compile_options(${LIBRARY_NAME} PRIVATE -Wundefined-reinterpret-cast)
    target_compile_options(${LIBRARY_NAME} PUBLIC -mllvm -inline-threshold=600)
    target_compile_options(${LIBRARY_NAME} PUBLIC -Wshadow)
    target_compile_options(${LIBRARY_NAME} PUBLIC -Wno-c++98-c++11-c++14-c++17-compat)

    set_source_files_properties(${TIFF_SOURCES} PROPERTIES COMPILE_FLAGS -Wno-unused-parameter)

    if (APPLE)
      target_compile_options(${LIBRARY_NAME} PUBLIC -stdlib=libc++)
      target_link_options(${LIBRARY_NAME} PUBLIC -stdlib=libc++)
    else ()
      target_compile_options(${LIBRARY_NAME} PUBLIC -fsized-deallocation)
    endif ()
  elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(${TIFF_SOURCES} PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -Wno-unused-but-set-variable")
  endif ()

  message(STATUS "ATLAS_SANITIZE_ADDRESS: ${ATLAS_SANITIZE_ADDRESS}")
  if (ATLAS_SANITIZE_ADDRESS)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
      target_compile_options(${LIBRARY_NAME} PUBLIC -fsanitize=address,undefined,integer,nullability -fno-omit-frame-pointer -fno-sanitize=unsigned-integer-overflow)
      target_link_options(${LIBRARY_NAME} PUBLIC -fsanitize=address,undefined,integer,nullability -fno-omit-frame-pointer -fno-sanitize=unsigned-integer-overflow)
    else ()
      target_compile_options(${LIBRARY_NAME} PUBLIC -fsanitize=address,undefined,integer,nullability -fno-omit-frame-pointer -fno-sanitize=unsigned-integer-overflow)
      target_link_options(${LIBRARY_NAME} PUBLIC -fsanitize=address,undefined,integer,nullability -fno-omit-frame-pointer -fno-sanitize=unsigned-integer-overflow)
    endif ()
  endif ()

  # for some files
  set_source_files_properties(zimageavx512.cpp PROPERTIES COMPILE_FLAGS -mavx512f)
  set_source_files_properties(zsaturateoperation_avx512.cpp PROPERTIES COMPILE_FLAGS -mavx512bw)
  set_source_files_properties(zsaturateoperation_avx2.cpp PROPERTIES COMPILE_FLAGS -mavx2)
  set_source_files_properties(zimageavx.cpp PROPERTIES COMPILE_FLAGS -mavx)
  set_source_files_properties(zimagesse3.cpp PROPERTIES COMPILE_FLAGS -msse3)

  set_source_files_properties(ztiff.cpp PROPERTIES COMPILE_FLAGS -I${TIFFDIR})
  set_source_files_properties(zimgpng.cpp PROPERTIES COMPILE_FLAGS -I${PNG_INCLUDE_DIRS})
  foreach (item IN LISTS JPEGXR_INCLUDE_DIRS)
    message(STATUS "JPEGXR_INCLUDE_DIRS: ${item}")
    set(JXRINC "${JXRINC} -I${item}")
  endforeach ()
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_FLAGS ${JXRINC})
  set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_DEFINITIONS "__ANSI__;DISABLE_PERF_MEASUREMENT")
  set_source_files_properties(zimgitkimage.cpp PROPERTIES COMPILE_FLAGS -I${SCIFIODIR})
  set_source_files_properties(zimginterface.cpp PROPERTIES COMPILE_FLAGS -I${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/magic_enum/include)
  # set_source_files_properties(zroiutils2.cpp PROPERTIES COMPILE_FLAGS -I${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/include/skia)

  set_source_files_properties(zregistrationoptimizer.cpp PROPERTIES COMPILE_FLAGS -Wno-shadow)
endif (WIN32)

target_link_libraries(${LIBRARY_NAME} PUBLIC TBB::tbb ${ITK_LIBRARIES} ${JPEGTURBO_LIBRARIES}
                      png_static ${JPEGXR_LIBRARIES} ${FREEIMAGE_LIBRARIES} cpuinfo::cpuinfo
                      hdf5_cpp-static hdf5-static Ceres::ceres Folly::folly Eigen3::Eigen Boost::headers
                      GeometricTools glm::glm
                      WebP::webp zstd::libzstd_static liblzma::liblzma ZLIB::ZLIB
                      ${VTK_LIBRARIES} assimp::assimp
                      glog::glog llfio::sl ${_QT_LIBS_})

if (UNIX AND NOT APPLE)
  target_link_libraries(${LIBRARY_NAME} PUBLIC -Wl,--start-group ${MKL_LIBRARIES} ${IPP_LIBRARIES} -Wl,--end-group)
else ()
  target_link_libraries(${LIBRARY_NAME} PUBLIC ${MKL_LIBRARIES} ${IPP_LIBRARIES})
endif ()

# target_include_directories(${LIBRARY_NAME} SYSTEM BEFORE PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/include)
