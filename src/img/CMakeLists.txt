cmake_minimum_required(VERSION 3.17.5)
project(img)

# main stuff

# tiff srcs
set(TIFFDIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libtiff/libtiff)
set(TIFF_ATLAS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libtiff_atlas/include)
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
list(APPEND TIFF_SOURCES ${TIFFDIR}/tif_unix.c)
source_group(libtiff FILES ${TIFF_SOURCES})

set(protobuf_PROTOC_EXE protobuf::protoc)
set(BIOFORMATS_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/../protos/bioformats_bridge.proto)
protobuf_generate(LANGUAGE cpp APPEND_PATH PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR} OUT_VAR BIOFORMATS_PROTO_SRCS
                  PROTOS ${BIOFORMATS_PROTO})
include(gRPCGenerateCPP)
grpc_generate_cpp(BIOFORMATS_GRPC_SRCS BIOFORMATS_GRPC_HDRS ${CMAKE_CURRENT_BINARY_DIR} ${BIOFORMATS_PROTO})
set_property(SOURCE ${BIOFORMATS_PROTO_SRCS} ${BIOFORMATS_GRPC_SRCS} PROPERTY SKIP_AUTOGEN ON)

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
        zbioformatsbridgeclient.h zbioformatsbridgeclient.cpp
        zimgbioformats.h zimgbioformats.cpp
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
        zimgtiff.h zimgtiff.cpp
        zimgv3draw.h zimgv3draw.cpp
        zimgzeissczi.h zimgzeissczi.cpp
        zimgzeisslsm.h zimgzeisslsm.cpp
        zioutils.h zioutils.cpp
        zioreadstats.h zioreadstats.cpp
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
        zabslflagtypes.h
        zcommandlineflags.h zcommandlineflags.cpp
        zglogbridge.h zglogbridge.cpp
        zlog.h zlog.cpp
        zwindowsheader.h
        zflags.h
        zimghdf5.cpp zimghdf5.h
        zhdf5globallock.cpp zhdf5globallock.h
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
        zswcgeom.cpp zswcgeom.h
        zswcloaderlegacy.cpp zswcloaderlegacy.h
        zswcnodeops.h
        zswcops.cpp zswcops.h
        zswceditlegacy.cpp zswceditlegacy.h
        zarrayfilters.cpp zarrayfilters.h
        zswcpointdist.cpp zswcpointdist.h
        zswcpostprocess.cpp zswcpostprocess.h
        zswcsubtract.cpp zswcsubtract.h
        zswcrescale.cpp zswcrescale.h
        zswcresampler.cpp zswcresampler.h
        zswctreenodegeomlegacy.cpp zswctreenodegeomlegacy.h
        zswcwriter.cpp zswcwriter.h
        zswcspatialindex.cpp zswcspatialindex.h
        zswcgeometrymaskvolume.cpp zswcgeometrymaskvolume.h
        zblockedautotracesession.cpp zblockedautotracesession.h
        zskeleton.h zskeleton.cpp
        ztree.hpp
        zvoxelregion.h zvoxelregion.cpp
        zimgtile.h zimgtile.cpp
        zimgitkinterface.h
        zcompleximg.h zcompleximg.cpp
        zfft.h zfft.cpp
        zvoxelvolume.h
        zvoxelvolumedense.h
        zsparsevoxelmask.h
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
        zimgbinaryopslegacy.h zimgbinaryopslegacy.cpp
        zimgbwthinlegacy.h zimgbwthinlegacy.cpp
        zimgmedianfilterlegacy.h zimgmedianfilterlegacy.cpp
        zimgsigneddistancemap.h zimgsigneddistancemap.cpp
        zimggraph.h zimggraph.cpp
        zimgnccmatch.h zimgnccmatch.cpp
        zimgfillhole.h zimgfillhole.cpp
        zimgregistration.h zimgregistration.cpp
        zregistrationoptimizer.h zregistrationoptimizer.cpp
        zregistrationcostfunction.h zregistrationcostfunction.cpp
        zregistrationnumericdiffcostfunction.h zregistrationnumericdiffcostfunction.cpp
        zimgprocess.h zimgprocess.cpp
        zbinarizeimage.h zbinarizeimage.cpp
        zsubtractbackground.h zsubtractbackground.cpp
        zsubtractbackgroundadaptive.h zsubtractbackgroundadaptive.cpp
        zenhanceline.h zenhanceline.cpp
        zassignpuncta.h zassignpuncta.cpp
        zchromaticshiftcorrection.cpp zchromaticshiftcorrection.h
        zpunctadetection.h zpunctadetection.cpp
        zsectionsregistration.h zsectionsregistration.cpp
        zstitchimage.cpp zstitchimage.h
        zimginit.h zimginit.cpp
        zjson.h zjson.cpp
        znaturalcubicspline2d.h znaturalcubicspline2d.cpp
        zroimaskrasterizer.cpp zroimaskrasterizer.h
        zminimumspanningtree.cpp zminimumspanningtree.h
        zmkl.h
        zsaturateoperation.cpp
        zsaturateoperation_avx2.cpp zsaturateoperation_avx512.cpp zsaturateoperation_avx2.h zsaturateoperation_avx512.h
        zimageavx512.h zimageavx512.cpp
        zmesh.h zmesh.cpp
        zmeshio.h zmeshio.cpp
        zmeshutils.h
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
    zneutubedarrayqsort.cpp zneutubedarrayqsort.h
    zneutubedarraymath.cpp zneutubedarraymath.h
    zneutubemathutils.h
    zneutubecontfun.h
    zneutube3dgeom.cpp zneutube3dgeom.h
    zneutubecompareswc.cpp zneutubecompareswc.h
    zneutubegeneral.cpp zneutubegeneral.h
    zneutubeneurocompconn.cpp zneutubeneurocompconn.h
    zneutubegeo3dpointarray.cpp zneutubegeo3dpointarray.h
    zneutubegeo3dcircle.h
    zneutubegeo3dellipse.cpp zneutubegeo3dellipse.h
    zneutubegeo3dscalarfield.cpp zneutubegeo3dscalarfield.h
    zneutubegeo3dutils.cpp zneutubegeo3dutils.h
    zneutubeedt3d.cpp zneutubeedt3d.h
    zneutubegraph.cpp zneutubegraph.h
    zneutubeinthistogram.cpp zneutubeinthistogram.h
    zneutubeimgbwmorph.cpp zneutubeimgbwmorph.h
    zneutubeimgbinarizer.cpp zneutubeimgbinarizer.h
    zneutubeimglocmax.cpp zneutubeimglocmax.h
    zneutubeimgsampling.cpp zneutubeimgsampling.h
    zneutubeneighborhood.cpp zneutubeneighborhood.h
    zneutubeneuroseg.cpp zneutubeneuroseg.h
    zneutubeneuronstructure.cpp zneutubeneuronstructure.h
    zneutubeobjlabel.cpp zneutubeobjlabel.h
    zneutubeoptimizeutils.cpp zneutubeoptimizeutils.h
    zneutubeperceptor.cpp zneutubeperceptor.h
    zneutubeplanaredt.cpp zneutubeplanaredt.h
    zneutubespgrow.cpp zneutubespgrow.h
    zneutubespgrowparser.cpp zneutubespgrowparser.h
    zneutubetrace.cpp zneutubetrace.h
    zneutubetraceinteractive.cpp zneutubetraceinteractive.h
    zneutubetraceauto.cpp zneutubetraceauto.h
    zneutubetraceconfig.cpp zneutubetraceconfig.h
    zneutubeautotraceprocess.cpp zneutubeautotraceprocess.h
    zneutubeblockedautotraceprocess.cpp zneutubeblockedautotraceprocess.h
    zneutubeskeletonizeprocess.cpp zneutubeskeletonizeprocess.h
    zneutubetraceconnectiontestworkspace.cpp zneutubetraceconnectiontestworkspace.h
    zneutubetraceconnectiontest.cpp zneutubetraceconnectiontest.h
    zneutubetracedefs.h
    zneutubetracerecord.cpp zneutubetracerecord.h
    zneutubetracemask.cpp zneutubetracemask.h
    zneutubetraceseed.cpp zneutubetraceseed.h
    zneutubetraceseeder.cpp zneutubetraceseeder.h
    zneutubetraceallseeds.cpp zneutubetraceallseeds.h
    zneutubetracescorethresholds.cpp zneutubetracescorethresholds.h
    zneutubetracerecover.cpp zneutubetracerecover.h
    zneutubetracechainscreen.cpp zneutubetracechainscreen.h
    zneutubetracelocseglabel.cpp zneutubetracelocseglabel.h
    zneutubestackfitoptions.h
    zneutubestackfitscore.cpp zneutubestackfitscore.h
    zneutubestackgraph.cpp zneutubestackgraph.h
    zneutubestackroute.cpp zneutubestackroute.h
    zneutubetraceworkspace.cpp zneutubetraceworkspace.h
    zneutubeskeletonize.cpp zneutubeskeletonize.h
    zneutubeskeletonizer.cpp zneutubeskeletonizer.h
    zneutubeswcsignalfitter.cpp zneutubeswcsignalfitter.h
    zneutubeswcreconnect.cpp zneutubeswcreconnect.h
    zneutubeswcregionsampling.cpp zneutubeswcregionsampling.h
    zneutubetraceconnect.cpp zneutubetraceconnect.h
    zneutubetraceswclocseg.cpp zneutubetraceswclocseg.h
    zneutubetraceswclabelstack.cpp zneutubetraceswclabelstack.h
    zneutubetracep2p.cpp zneutubetracep2p.h
    zneutubevoxel.h
    zneutubevoxelarray.cpp zneutubevoxelarray.h
    zneutubelocalneuroseg.cpp zneutubelocalneuroseg.h
    zneutubelocsegchain.cpp zneutubelocsegchain.h
    zneutubelocsegchaincircle.cpp zneutubelocsegchaincircle.h
    zneutubelocsegchaininterpolate.cpp zneutubelocsegchaininterpolate.h
    zneutubelocsegchainknot.cpp zneutubelocsegchainknot.h
    zneutubelocsegchainmetrics.cpp zneutubelocsegchainmetrics.h
    zneutubelocsegchainshortestpath.cpp zneutubelocsegchainshortestpath.h
    zneutubelocsegchaintrace.cpp zneutubelocsegchaintrace.h
    zrunneutucommand2.cpp zrunneutucommand2.h
)

if (NOT ZIMG_DISABLE_FREEIMAGE)
    list(APPEND IMG_SOURCES zimgfreeimage.h zimgfreeimage.cpp)
endif ()

message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "CMAKE_LINKER: ${CMAKE_LINKER}")

# our target
set(LIBRARY_NAME img)
add_library(${LIBRARY_NAME} STATIC ${TIFF_SOURCES} ${IMG_SOURCES} ${BIOFORMATS_PROTO_SRCS} ${BIOFORMATS_GRPC_SRCS})
set_property(TARGET ${LIBRARY_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ${ATLAS_ENABLE_RELEASE_IPO})

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
target_include_directories(${LIBRARY_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
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
set_source_files_properties(${TIFF_SOURCES} ztiff.cpp PROPERTIES INCLUDE_DIRECTORIES "${TIFF_ATLAS_INCLUDE_DIR};${TIFFDIR}")
set_source_files_properties(${TIFFDIR}/tif_open.c PROPERTIES COMPILE_DEFINITIONS ALLOW_TIFF_NON_EXT_ALLOC_FUNCTIONS)
# set_source_files_properties(${TIFFDIR}/tif_win32.c PROPERTIES COMPILE_DEFINITIONS ALLOW_TIFF_NON_EXT_ALLOC_FUNCTIONS)
set_source_files_properties(${TIFFDIR}/tif_unix.c PROPERTIES COMPILE_DEFINITIONS ALLOW_TIFF_NON_EXT_ALLOC_FUNCTIONS)
set_source_files_properties(zimgpng.cpp PROPERTIES INCLUDE_DIRECTORIES "${PNG_INCLUDE_DIRS}")
if (NOT ZIMG_DISABLE_FREEIMAGE)
    set_source_files_properties(zimgfreeimage.cpp PROPERTIES INCLUDE_DIRECTORIES "${FREEIMAGE_INCLUDE_DIRS}")
endif ()
set_source_files_properties(zimgjpegxr.cpp PROPERTIES INCLUDE_DIRECTORIES "${JPEGXR_INCLUDE_DIRS}")
set_source_files_properties(zimginterpolate.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/ITKMorphologicalContourInterpolation")
set(BIOFORMATS_BRIDGE_SOURCE_FILES
        zbioformatsbridgeclient.cpp
        ${BIOFORMATS_PROTO_SRCS}
        ${BIOFORMATS_GRPC_SRCS})
set_source_files_properties(${BIOFORMATS_BRIDGE_SOURCE_FILES}
        PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/include;${CMAKE_CURRENT_BINARY_DIR}")

# The generated protobuf headers include protobuf runtime headers with quotes
# and require an exact gencode/header version match. Keep those quote includes
# on Atlas' bundled protobuf first: CMake's SYSTEM include for 3rdparty/build
# becomes -isystem, which AppleClang searches after its default /usr/local/include.
if (MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_property(SOURCE ${BIOFORMATS_BRIDGE_SOURCE_FILES}
            APPEND PROPERTY COMPILE_OPTIONS "/clang:-iquote${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/include")
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_property(SOURCE ${BIOFORMATS_BRIDGE_SOURCE_FILES}
            APPEND PROPERTY COMPILE_OPTIONS "-iquote${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/include")
endif ()

if (WIN32)
    target_compile_definitions(${LIBRARY_NAME} PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
            UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00
            _USE_MATH_DEFINES
            _SILENCE_CXX17_RESULT_OF_DEPRECATION_WARNING
            _SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING
            _HAS_DEPRECATED_RESULT_OF)

    target_compile_options(${LIBRARY_NAME} PUBLIC /GR) # Enable Run-Time Type Information
    target_compile_options(${LIBRARY_NAME} PUBLIC /EHsc) # exception handling model, https://msdn.microsoft.com/en-us/library/1deeycx5.aspx
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
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd4866) # compiler may not enforce left-to-right evaluation order for overloaded comma operator
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd5039) # 'function': pointer or reference to potentially throwing function passed to extern C function under -EHc. Undefined behavior may occur if this function throws an exception.
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd5204) # A class with virtual functions has non-virtual trivial destructor.
    target_compile_options(${LIBRARY_NAME} PUBLIC /wd5246) # 'member': the initialization of a subobject should be wrapped in braces
    target_compile_options(${LIBRARY_NAME} PUBLIC /utf-8)  # https://blogs.msdn.microsoft.com/vcblog/2016/02/22/new-options-for-managing-character-sets-in-the-microsoft-cc-compiler/
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${LIBRARY_NAME} PUBLIC /Wall)
        target_compile_options(${LIBRARY_NAME} PUBLIC /analyze) # The C/C++ Code Analysis tool provides information to developers about possible defects in their C/C++ source code
        # target_compile_options(${LIBRARY_NAME} PUBLIC /wd6255) # _alloca indicates failure by raising a stack overflow exception. Consider using _malloca instead.
        target_compile_options(${LIBRARY_NAME} PUBLIC /wd6326) # Potential comparison of a constant with another constant.
        # target_compile_options(${LIBRARY_NAME} PUBLIC /wd6330) # Incorrect type passed as parameter in call to function.
        target_compile_options(${LIBRARY_NAME} PUBLIC /wd6386) # Buffer overrun while writing to 'data'.
        target_compile_options(${LIBRARY_NAME} PUBLIC /wd28020)
        target_compile_options(${LIBRARY_NAME} PUBLIC /wd6294) # Ill-defined for-loop: initial condition does not satisfy test, loop body not executed.
        target_compile_options(${LIBRARY_NAME} PUBLIC /wd6239) #  (<non-zero constant> && <expression>) always evaluates to the result of <expression>. Did you intend to use the bitwise-and operator?
        target_compile_options(${LIBRARY_NAME} PUBLIC /wd6336) # Arithmetic operator has precedence over question operator, use parentheses to clarify intent.
        target_compile_options(${LIBRARY_NAME} PUBLIC /wd6255) # _alloca indicates failure by raising a stack overflow exception.  Consider using _malloca instead.
        target_compile_options(${LIBRARY_NAME} PUBLIC /wd6262) # Function uses constant_1 bytes of stack: exceeds /analyze:stacksize constant_2. Consider moving some data to heap
        target_compile_options(${LIBRARY_NAME} PUBLIC /wd6385) # Invalid data: accessing buffer-name, the readable size is size1 bytes, but size2 bytes may be read: Lines: x, y
        target_compile_options(${LIBRARY_NAME} PUBLIC /MP) # Build with Multiple Processes
        # standard conformance
        target_compile_options(${LIBRARY_NAME} PUBLIC /permissive- /Zc:preprocessor /Zc:lambda /Zc:externConstexpr /Zc:throwingNew /Zc:enumTypes /Zc:templateScope)
    else ()
        target_compile_options(${LIBRARY_NAME} PUBLIC /clang:-Wall /clang:-Wextra)
        target_compile_options(${LIBRARY_NAME} PUBLIC /clang:-Wno-c++98-c++11-c++14-c++17-compat)
        target_compile_options(${LIBRARY_NAME} PUBLIC /clang:-Wno-unsafe-buffer-usage)
        target_compile_options(${LIBRARY_NAME} PUBLIC /clang:-Wno-switch-default)
        target_compile_options(${LIBRARY_NAME} PUBLIC /clang:-Wno-float-conversion)
        target_compile_options(${LIBRARY_NAME} PUBLIC /clang:-Wno-missing-field-initializers)
        # Keep clang-cl aligned with non-Windows Clang for Eigen-heavy img code.
        target_compile_options(${LIBRARY_NAME} PUBLIC /clang:-mllvm /clang:-inline-threshold=600)
        set_source_files_properties(${TIFF_SOURCES} PROPERTIES COMPILE_FLAGS "/clang:-Wno-reserved-identifier /clang:-Wno-unused-parameter /clang:-Wno-unused-command-line-argument /clang:-Wno-sign-conversion /clang:-Wno-format")
    endif ()
    # standard conformance
    target_compile_options(${LIBRARY_NAME} PUBLIC /volatile:iso /Zc:inline)
    target_compile_options(${LIBRARY_NAME} PUBLIC /Zc:__cplusplus) # The /Zc:__cplusplus compiler option enables the __cplusplus preprocessor macro to report an updated value for recent C++ language standards support

    target_compile_options(${LIBRARY_NAME} PUBLIC /arch:AVX)
    target_link_options(${LIBRARY_NAME} PUBLIC /FORCE:MULTIPLE)

    # for some files
    set_source_files_properties(zimageavx512.cpp PROPERTIES COMPILE_FLAGS /arch:AVX512)
    set_source_files_properties(zsaturateoperation_avx512.cpp PROPERTIES COMPILE_FLAGS /arch:AVX512)
    set_source_files_properties(zsaturateoperation_avx2.cpp PROPERTIES COMPILE_FLAGS /arch:AVX2)

    set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_DEFINITIONS "WIN32;DISABLE_PERF_MEASUREMENT")
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
        target_compile_options(${LIBRARY_NAME} PUBLIC -Wno-missing-field-initializers)
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

    set_source_files_properties(zimgjpegxr.cpp PROPERTIES COMPILE_DEFINITIONS "__ANSI__;DISABLE_PERF_MEASUREMENT")

    set_source_files_properties(zregistrationoptimizer.cpp PROPERTIES COMPILE_FLAGS -Wno-shadow)
endif (WIN32)

if (NOT ZIMG_DISABLE_FREEIMAGE)
    target_link_libraries(${LIBRARY_NAME} PUBLIC ${FREEIMAGE_LIBRARIES})
endif ()

target_link_libraries(${LIBRARY_NAME} PUBLIC ${ITK_LIBRARIES} ${JPEGTURBO_LIBRARIES}
        png_static ${JPEGXR_LIBRARIES} cpuinfo::cpuinfo
        hdf5_cpp-static hdf5-static Ceres::ceres Folly::folly Eigen3::Eigen
        Boost::headers Boost::charconv
        absl::flags_parse absl::flags_reflection absl::flags_usage
        absl::check absl::failure_signal_handler absl::log absl::log_globals
        absl::log_initialize absl::log_sink absl::log_sink_registry absl::symbolize
        glm::glm-header-only
        WebP::webp zstd::libzstd_static liblzma::liblzma ZLIB::ZLIB
        ${VTK_LIBRARIES} assimp::assimp TBB::tbb
        glog::glog gRPC::grpc++ protobuf::libprotobuf ${_QT_LIBS_})

if (llfio_FOUND)
    target_link_libraries(${LIBRARY_NAME} PUBLIC llfio::sl)
endif ()

if (UNIX AND NOT APPLE)
    target_link_libraries(${LIBRARY_NAME} PUBLIC -Wl,--start-group ${MKL_LIBRARIES} -Wl,--end-group)
else ()
    target_link_libraries(${LIBRARY_NAME} PUBLIC ${MKL_LIBRARIES})
endif ()

if (APPLE)
    # When building a universal2 binary, some dependencies may be architecture-specific (e.g. Intel oneAPI MKL is
    # x86_64-only). The per-arch link step intentionally ignores the non-matching slices, and Apple ld emits noisy
    # "found architecture 'x86_64', required architecture 'arm64'" warnings. Suppress those warnings.
    list(LENGTH CMAKE_OSX_ARCHITECTURES ATLAS_OSX_ARCH_COUNT)
    if (ATLAS_OSX_ARCH_COUNT GREATER 1)
        # target_link_options(${LIBRARY_NAME} BEFORE PUBLIC -Wl,-no_arch_warnings) # not working, have to use:
        target_link_options(${LIBRARY_NAME} PUBLIC -Wl,-w) # todo: need better solution
    endif ()
    target_link_libraries(${LIBRARY_NAME} PUBLIC "-framework Accelerate")
endif ()
