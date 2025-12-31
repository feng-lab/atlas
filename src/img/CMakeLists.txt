cmake_minimum_required(VERSION 3.17.5)
project(img)

# main stuff

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
        ${TIFFDIR}/tif_jpeg_12.c
        ${TIFFDIR}/tif_lzma.c
        ${TIFFDIR}/tif_zstd.c
        ${TIFFDIR}/tif_webp.c
        ${TIFFDIR}/tif_stream.cxx
        ${TIFFDIR}/tif_hash_set.c
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
        zbenchtimer.h zbenchtimer.cpp
        zbitset.h
        zcpuinfo.h zcpuinfo.cpp
        zeigenutils.h zeigenutils.cpp
        zglmutils.h
        zexception.h zexception.cpp
        zglobal.h
        zimage2dutils.h zimage2dutils.cpp
        zimage3dutils.h
        zimageavx.h zimageavx.cpp
        zimagefilterkernel.h zimagefilterkernel.cpp
        zimagesse3.h zimagesse3.cpp
        zimg.h zimg.cpp
        zimgformat.h zimgformat.cpp
        zimginfo.h zimginfo.cpp
        zimginterface.h zimginterface.cpp
        zimgio.h zimgio.cpp
        zimgitkimage.h zimgitkimage.cpp
        zimgjpeg.h zimgjpeg.cpp
        zimgjpegxr.h zimgjpegxr.cpp
        zimgmetadatabase.h
        zimgmetaimage.h zimgmetaimage.cpp
        zimgmetatag.h zimgmetatag.cpp
        zimgometiff.h zimgometiff.cpp
        zimgpng.h zimgpng.cpp
        zimgregion.h zimgregion.cpp
        zimgsliceprovider.h zimgsliceprovider.cpp
        # zimgstackinterface.h
        zimgtiff.h zimgtiff.cpp
        zimgv3draw.h zimgv3draw.cpp
        zimgzeissczi.h zimgzeissczi.cpp
        zimgzeisslsm.h zimgzeisslsm.cpp
        zioutils.h zioutils.cpp
        zrandom.h zrandom.cpp
        zsaturateoperation.h
        zstatisticsutils.h
        zstringutils.h zstringutils.cpp
        ztiff.h ztiff.cpp
        zvoxelcoordinate.h
        zimgregioniterator.h
        zimgneighborhooditerator.h
        zimgneighborhoodwithcoorditerator.h
        zneighborhood.h zneighborhood.cpp
        zimgneighborhoodwithptriterator.h
        zlog.h zlog.cpp
        zwindowsheader.h
        # zimgstackinterface.cpp
        zflags.h
        zimghdf5.cpp zimghdf5.h
        zimginfoio.cpp zimginfoio.h
        zimgblockprovider.h
        zimgleica.cpp zimgleica.h
        zgmm.h
        zkmeans.h
        zvbgmm.h
        zpuncta.cpp zpuncta.h
        zpunctaio.cpp zpunctaio.h
        zpunctum.cpp zpunctum.h
        zswc.h zswc.cpp
        ztree.hpp
        zvoxelregion.h zvoxelregion.cpp
        zimgtile.h zimgtile.cpp
        zimgitkinterface.h
        zcompleximg.h zcompleximg.cpp
        zfft.h zfft.cpp
        zaffine2d.h zaffine2d.cpp
        zaffine3d.h zaffine3d.cpp
        zimageinterpolation.h zimageinterpolation.cpp
        zimagetoimagemetric.h zimagetoimagemetric.cpp
        zimagetransform.h zimagetransform.cpp
        zimagematrix2dtransform.h zimagematrix2dtransform.cpp
        zimagematrix3dtransform.h zimagematrix3dtransform.cpp
        zimagecompositetransform.h zimagecompositetransform.cpp
        zimagetransformresolve.h zimagetransformresolve.cpp
        zimgmerge.h zimgmerge.cpp
        zimgncc.h zimgncc.cpp
        zimgalgorithm.h zimgalgorithm.cpp
        zimgconnectedcomponents.h zimgconnectedcomponents.cpp
        zimgregionalextrema.h zimgregionalextrema.cpp
        zimgautothreshold.h zimgautothreshold.cpp
        zimgsigneddistancemap.h zimgsigneddistancemap.cpp
        zimggraph.h zimggraph.cpp
        zimgnccmatch.h zimgnccmatch.cpp
        zimgfillhole.h zimgfillhole.cpp
        zimgregistration.h zimgregistration.cpp
        zregistrationoptimizer.h zregistrationoptimizer.cpp
        zregistrationcostfunction.h zregistrationcostfunction.cpp
        zregistrationnumericdiffcostfunction.h zregistrationnumericdiffcostfunction.cpp
        zimgprocess.h zimgprocess.cpp
        zassignpuncta.h zassignpuncta.cpp
        zchromaticshiftcorrection.cpp zchromaticshiftcorrection.h
        zpunctadetection.h zpunctadetection.cpp
        zsectionsregistration.h zsectionsregistration.cpp
        zstitchimage.cpp zstitchimage.h
        zimginit.h zimginit.cpp
        zjson.h zjson.cpp
        zroiutils.cpp zroiutils.h
        zlogcache.cpp zlogcache.h
        zminimumspanningtree.cpp zminimumspanningtree.h
        zmkl.h
        zsaturateoperation.cpp
        zsaturateoperation_avx2.cpp zsaturateoperation_avx512.cpp zsaturateoperation_avx2.h zsaturateoperation_avx512.h
        zimageavx512.h zimageavx512.cpp
        zmesh.h zmesh.cpp
        zmeshio.h zmeshio.cpp
        zmeshutils.h zmeshutils.cpp
        zimginterpolate.h zimginterpolate.cpp
        zimgblockprovider.cpp
        zmemorymappedfile.cpp zmemorymappedfile.h
        zmemorymappedfilecache.cpp zmemorymappedfilecache.h
        zh5zjpegxr.cpp zh5zjpegxr.h
        zhashutils.h
        zfolly.cpp zfolly.h
        zcachedimg.cpp zcachedimg.h
        zstructutils.h
        zwindowsheader.cpp
        zcancellation.h
        zconcurrentlrucache.h
)

if (NOT ZIMG_DISABLE_FREEIMAGE)
    list(APPEND IMG_SOURCES zimgfreeimage.h zimgfreeimage.cpp)
endif ()

message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")

# our target
set(LIBRARY_NAME img)
add_library(${LIBRARY_NAME} STATIC ${TIFF_SOURCES} ${SCIFIO_SOURCES} ${IMG_SOURCES})

if (NOT ZIMG_DISABLE_FREEIMAGE)
    target_compile_definitions(${LIBRARY_NAME} PUBLIC ZIMG_HAVE_FREEIMAGE)
endif ()

# mkl
target_compile_definitions(${LIBRARY_NAME} PUBLIC ZIMG_USE_MKL)
target_include_directories(${LIBRARY_NAME} SYSTEM PRIVATE ${MKL_INCLUDE_DIRS})

if (llfio_FOUND)
    target_compile_definitions(${LIBRARY_NAME} PUBLIC ZIMG_USE_LLFIO)
endif ()

if (WIN32)
    target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/include)
endif ()
target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if (NOT WIN32)
    target_include_directories(${LIBRARY_NAME} SYSTEM BEFORE PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/include)
endif ()
target_compile_definitions(${LIBRARY_NAME} PUBLIC EIGEN_NO_DEBUG EIGEN_DONT_PARALLELIZE EIGEN_USE_BLAS)

target_compile_definitions(${LIBRARY_NAME} PUBLIC lapack_complex_float=std::complex<float>
        lapack_complex_double=std::complex<double>)
target_compile_definitions(${LIBRARY_NAME} PUBLIC QT_NO_KEYWORDS QT_USE_QSTRINGBUILDER QT_NO_CAST_FROM_BYTEARRAY
        QT_NO_CAST_TO_ASCII QT_NO_VERSION_TAGGING)
target_compile_definitions(${LIBRARY_NAME} PUBLIC BOOST_CONTAINER_NO_LIB BOOST_JSON_NO_LIB)

target_compile_definitions(${LIBRARY_NAME} PRIVATE TIFF_DO_NOT_USE_NON_EXT_ALLOC_FUNCTIONS)
set_source_files_properties(${TIFFDIR}/tif_open.c PROPERTIES COMPILE_DEFINITIONS ALLOW_TIFF_NON_EXT_ALLOC_FUNCTIONS)
# set_source_files_properties(${TIFFDIR}/tif_win32.c PROPERTIES COMPILE_DEFINITIONS ALLOW_TIFF_NON_EXT_ALLOC_FUNCTIONS)
set_source_files_properties(${TIFFDIR}/tif_unix.c PROPERTIES COMPILE_DEFINITIONS ALLOW_TIFF_NON_EXT_ALLOC_FUNCTIONS)

if (WIN32)
    target_compile_definitions(${LIBRARY_NAME} PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
            UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00
            _USE_MATH_DEFINES
            _SILENCE_CXX17_RESULT_OF_DEPRECATION_WARNING
            _SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING
            _HAS_DEPRECATED_RESULT_OF)

    target_compile_options(${LIBRARY_NAME} PUBLIC /GR) # Enable Run-Time Type Information
    target_compile_options(${LIBRARY_NAME} PUBLIC /EHsc) # exception handling model, https://msdn.microsoft.com/en-us/library/1deeycx5.aspx
    target_compile_options(${LIBRARY_NAME} PUBLIC /Wall)
    target_compile_options(${LIBRARY_NAME} PUBLIC /bigobj) # increases the number of sections that an object file can contain.
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd4267) # 'var' : conversion from 'size_t' to 'type', possible loss of data
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd4244) # 'argument' : conversion from 'type1' to 'type2', possible loss of data
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd4305) # 'identifier' : truncation from 'type1' to 'type2'
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd4819) # The file contains a character that cannot be represented in the current code page (number). Save the file in Unicode format to prevent data loss.
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd4828) # The file contains a character starting at offset 0x60 that is illegal in the current source character set (codepage 65001).
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd5105) # macro expansion producing 'defined' has undefined behavior, from windows sdk
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd5219) # implicit conversion from 'type-1' to 'type-2', possible loss of data
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd4242) # 'identifier' : conversion from 'type1' to 'type2', possible loss of data
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd4464) # relative include path contains '..'
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd4266) # 'function': no override available for virtual member function from base 'type'; function is hidden
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd5039) # 'function': pointer or reference to potentially throwing function passed to extern C function under -EHc. Undefined behavior may occur if this function throws an exception.
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd5204) # A class with virtual functions has non-virtual trivial destructor.
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd5246) # 'member': the initialization of a subobject should be wrapped in braces
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
        target_compile_options(${LIBRARY_NAME} PUBLIC /wd6262) # Function uses constant_1 bytes of stack: exceeds /analyze:stacksize constant_2. Consider moving some data to heap
        target_compile_options(${LIBRARY_NAME} PUBLIC /wd6385) # Invalid data: accessing buffer-name, the readable size is size1 bytes, but size2 bytes may be read: Lines: x, y
        target_compile_options(${LIBRARY_NAME} PUBLIC /MP) # Build with Multiple Processes
        target_compile_options(${LIBRARY_NAME} PUBLIC /GL) # Enables whole program optimization.
    else ()
        target_compile_options(${LIBRARY_NAME} PUBLIC /MD)
    endif ()
    # standard conformance
    target_compile_options(${LIBRARY_NAME} PUBLIC /permissive- /volatile:iso /Zc:inline /Zc:preprocessor /Zc:lambda)
    target_compile_options(${LIBRARY_NAME} PUBLIC /Zc:externConstexpr /Zc:throwingNew /Zc:enumTypes /Zc:templateScope)
    target_compile_options(${LIBRARY_NAME} PUBLIC /Zc:__cplusplus) # The /Zc:__cplusplus compiler option enables the __cplusplus preprocessor macro to report an updated value for recent C++ language standards support

    target_compile_options(${LIBRARY_NAME} PUBLIC /arch:AVX)
    target_link_options(${LIBRARY_NAME} PUBLIC /LTCG:STATUS /FORCE:MULTIPLE)

    # for some files
    set_source_files_properties(zimageavx512.cpp PROPERTIES COMPILE_FLAGS /arch:AVX512)
    set_source_files_properties(zsaturateoperation_avx512.cpp PROPERTIES COMPILE_FLAGS /arch:AVX512)
    set_source_files_properties(zsaturateoperation_avx2.cpp PROPERTIES COMPILE_FLAGS /arch:AVX2)

    set_source_files_properties(ztiff.cpp PROPERTIES COMPILE_FLAGS /I${TIFFDIR})
    set_source_files_properties(zimgpng.cpp PROPERTIES COMPILE_FLAGS /I${PNG_INCLUDE_DIRS})
    if (NOT ZIMG_DISABLE_FREEIMAGE)
        set_source_files_properties(zimgfreeimage.cpp PROPERTIES COMPILE_FLAGS /I${FREEIMAGE_INCLUDE_DIRS})
    endif ()
    foreach (item IN LISTS JPEGXR_INCLUDE_DIRS)
        message(STATUS "JPEGXR_INCLUDE_DIRS: ${item}")
        set(JXRINC "${JXRINC} /I${item}")
    endforeach ()
    set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_FLAGS ${JXRINC})
    set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_DEFINITIONS "WIN32;DISABLE_PERF_MEASUREMENT")
    set_source_files_properties(zimgitkimage.cpp PROPERTIES COMPILE_FLAGS /I${SCIFIODIR})
    set_source_files_properties(zimginterpolate.cpp PROPERTIES COMPILE_FLAGS /I${SCIFIODIR})
    # set_source_files_properties(zroiutils2.cpp PROPERTIES COMPILE_FLAGS /I${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/include/skia)
else (WIN32)
    target_compile_options(${LIBRARY_NAME} PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-frtti>)
    target_compile_options(${LIBRARY_NAME} PUBLIC -fexceptions -fwrapv -fno-strict-aliasing)
    target_compile_options(${LIBRARY_NAME} PUBLIC -Wall -Wextra)
    target_compile_options(${LIBRARY_NAME} PUBLIC -Waddress -Wdeprecated)
    if (APPLE)
        target_compile_options(${LIBRARY_NAME} PUBLIC -mcpu=apple-m1 -mavx -fopenmp-simd -DSIMDE_ENABLE_OPENMP)
    else ()
        target_compile_options(${LIBRARY_NAME} PUBLIC -mavx -fopenmp-simd -DSIMDE_ENABLE_OPENMP)
    endif ()
    target_compile_options(${LIBRARY_NAME} PUBLIC -fno-common)

    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${LIBRARY_NAME} PUBLIC -Wbool-conversion -Wconstant-conversion -Wenum-conversion)
        target_compile_options(${LIBRARY_NAME} PUBLIC -Waddress-of-temporary)
        target_compile_options(${LIBRARY_NAME} PUBLIC -mllvm -inline-threshold=600)
        target_compile_options(${LIBRARY_NAME} PUBLIC -Wshadow)
        target_compile_options(${LIBRARY_NAME} PUBLIC -Wno-c++98-c++11-c++14-c++17-compat)
        target_compile_options(${LIBRARY_NAME} PRIVATE -Wundefined-reinterpret-cast)

        set_source_files_properties(${TIFF_SOURCES} PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -Wno-unused-command-line-argument")

        if (APPLE)
            target_compile_options(${LIBRARY_NAME} PUBLIC -stdlib=libc++)
            target_link_options(${LIBRARY_NAME} PUBLIC -stdlib=libc++)
        else ()
            target_compile_options(${LIBRARY_NAME} PUBLIC -fsized-deallocation)
        endif ()
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set_source_files_properties(${TIFF_SOURCES} PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -Wno-unused-but-set-variable")
    endif ()

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

    set_source_files_properties(ztiff.cpp PROPERTIES COMPILE_FLAGS -I${TIFFDIR})
    set_source_files_properties(zimgpng.cpp PROPERTIES COMPILE_FLAGS -I${PNG_INCLUDE_DIRS})
    foreach (item IN LISTS JPEGXR_INCLUDE_DIRS)
        message(STATUS "JPEGXR_INCLUDE_DIRS: ${item}")
        set(JXRINC "${JXRINC} -I${item}")
    endforeach ()
    set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_FLAGS ${JXRINC})
    set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_DEFINITIONS "__ANSI__;DISABLE_PERF_MEASUREMENT")
    set_source_files_properties(zimgitkimage.cpp PROPERTIES COMPILE_FLAGS -I${SCIFIODIR})
    set_source_files_properties(zimginterpolate.cpp PROPERTIES COMPILE_FLAGS -I${SCIFIODIR})

    set_source_files_properties(zregistrationoptimizer.cpp PROPERTIES COMPILE_FLAGS -Wno-shadow)
endif (WIN32)

if (NOT ZIMG_DISABLE_FREEIMAGE)
    target_link_libraries(${LIBRARY_NAME} PUBLIC ${FREEIMAGE_LIBRARIES})
endif ()

target_link_libraries(${LIBRARY_NAME} PUBLIC ${ITK_LIBRARIES} ${JPEGTURBO_LIBRARIES}
        png_static ${JPEGXR_LIBRARIES} cpuinfo::cpuinfo
        hdf5_cpp-static hdf5-static Ceres::ceres Folly::folly Eigen3::Eigen
        Boost::headers Boost::charconv absl::log_initialize
        GeometricTools glm::glm-header-only
        WebP::webp zstd::libzstd_static liblzma::liblzma ZLIB::ZLIB
        ${VTK_LIBRARIES} assimp::assimp TBB::tbb
        glog::glog ${_QT_LIBS_})

if (llfio_FOUND)
    target_link_libraries(${LIBRARY_NAME} PUBLIC llfio::sl)
endif ()

if (UNIX AND NOT APPLE)
    target_link_libraries(${LIBRARY_NAME} PUBLIC -Wl,--start-group ${MKL_LIBRARIES} -Wl,--end-group)
else ()
    target_link_libraries(${LIBRARY_NAME} PUBLIC ${MKL_LIBRARIES})
endif ()

if (APPLE)
    target_link_libraries(${LIBRARY_NAME} PUBLIC "-framework Accelerate")
endif ()

# target_include_directories(${LIBRARY_NAME} SYSTEM BEFORE PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/include)
