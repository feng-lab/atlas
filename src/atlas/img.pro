TEMPLATE = lib
CONFIG += staticlib

macx {
# 1. brew install nasm
# 2. (optional, if no mkl) brew install fftw --build-bottle
# 3. run "build" from 3rdparty folder
# 4. use official qt5
# or
# 4. use qt 4.8: brew install qt and change qmake specs to unsupported/macx-clang-libc++

# suppress warnings from 3rd party library, works for gcc and clang
QMAKE_CXXFLAGS += -isystem $$PWD/3rdparty -Wno-deprecated-register
}

win32 {
# patch libtiff win32 TIFFFdOpen
# 1. run "build" from 3rdparty folder
# 2. use official qt5
QMAKE_CXXFLAGS += /wd4267 /wd4244 /wd4305 /wd4819 /bigobj /GL
QMAKE_LFLAGS += /LTCG
DEFINES += _CRT_SECURE_NO_WARNINGS NOMINMAX _USE_MSVC2013_
INCLUDEPATH += $$PWD/3rdparty
}

#Qt5
isEqual(QT_MAJOR_VERSION,5) | greaterThan(QT_MAJOR_VERSION,5) {
isEqual(QT_MAJOR_VERSION,5) {
  lessThan(QT_MINOR_VERSION,4) {
    message("Cannot build Atlas with Qt version $${QT_VERSION}.")
    error("Use at least Qt 5.4.0.")
  }
}
    message("Qt 5")
    QT += concurrent
    DEFINES += _QT5_
    CONFIG += c++11
}

#Qt4
isEqual(QT_MAJOR_VERSION,4) {
    message("Qt 4")
    include(3rdparty/qjson/qjson.pri)
    QMAKE_CXXFLAGS += -std=c++11
}

CONFIG += rtti exceptions

CONFIG += static_libtiff

DEFINES *= QT_USE_QSTRINGBUILDER

HEADERS += \
    zimg.h \
    zimginterface.h \
    zimginfo.h \
    zimgmetatag.h \
    zimgformat.h \
    zimgv3draw.h \
    zimgstackinterface.h \
    zimgio.h \
    zimgtiff.h \
    ztiff.h \
    zimgzeisslsm.h \
    zimgometiff.h \
    zimgjpeg.h \
    zcpuinfo.h \
    zimageinterpolation.h \
    zimagefilterkernel.h \
    zimage3dutils.h \
    zimage2dutils.h \
    zimgdisplay.h \
    zimg.hxx \
    zimgmerge.h \
    zstatisticsutils.h \
    zimgtile.h \
    zvoxelcoordinate.h \
    zvoxelregion.h \
    zimgpng.h \
    zsaturateoperation.h \
    zimgregion.h \
    zbbox.h \
    zimagesse3.h \
    zimageavx.h \
    zexception.h \
    zimgmetadatabase.h \
    zimgregioniterator.h \
    zimgneighborhooditerator.h \
    zioutils.h \
    zimgneighborhoodwithcoorditerator.h \
    zneighborhood.h \
    zimgalgorithm.h \
    zimgconnectedcomponents.h \
    zimgregionalextrema.h \
    zimgautothreshold.h \
    zimgneighborhoodwithptriterator.h \
    zimgitkinterface.h \
    zimgprocess.h \
    zimgsigneddistancemap.h \
    zimggraph.h \
    zbitset.h \
    zimgfreeimage.h \
    zeigenutils.h \
    ztree.hpp \
    zrandom.h \
    zbenchtimer.h \
    zglmutils.h \
    zstringutils.h \
    zglobal.h \
    3rdparty/lodepng/lodepng.h \
    zimgmetaimage.h \
    zimgsliceprovider.h \
    zimgzeissczi.h \
    zimgjpegxr.h

SOURCES += \
    zimg.cpp \
    zimginfo.cpp \
    zimgmetatag.cpp \
    zimgformat.cpp \
    zimgv3draw.cpp \
    zimginterface.cpp \
    zimgio.cpp \
    zimgtiff.cpp \
    ztiff.cpp \
    zimgzeisslsm.cpp \
    zimgometiff.cpp \
    zimgjpeg.cpp \
    zcpuinfo.cpp \
    zimageinterpolation.cpp \
    zimagefilterkernel.cpp \
    zimage2dutils.cpp \
    zimgmerge.cpp \
    zvoxelregion.cpp \
    zimgpng.cpp \
    zimagetoimagemetric.cpp \
    zimgregion.cpp \
    zioutils.cpp \
    zneighborhood.cpp \
    zimgalgorithm.cpp \
    zimgprocess.cpp \
    zimggraph.cpp \
    zimgsigneddistancemap.cpp \
    zimgconnectedcomponents.cpp \
    zimgautothreshold.cpp \
    zimgregionalextrema.cpp \
    zimgfreeimage.cpp \
    zeigenutils.cpp \
    zrandom.cpp \
    zbenchtimer.cpp \
    zstringutils.cpp \
    3rdparty/lodepng/lodepng.cpp \
    zimgmetaimage.cpp \
    zimgsliceprovider.cpp \
    zimgzeissczi.cpp

contains(CONFIG, static_libtiff) {
    include(3rdparty/libtiff.pri)
} else {
    LIBS += -ltiff -llzma
}

include(3rdparty/QsLog/QsLog.pri)

macx {
    SSE3_SOURCES = zimagesse3.cpp
    sse3.name = sse3
    sse3.input = SSE3_SOURCES
    sse3.variable_out = OBJECTS
    sse3.output = ${QMAKE_VAR_OBJECTS_DIR}${QMAKE_FILE_IN_BASE}$${first(QMAKE_EXT_OBJ)}
    sse3.commands = $${QMAKE_CXX} $(CXXFLAGS) -msse3 $(INCPATH) -c ${QMAKE_FILE_IN} -o ${QMAKE_FILE_OUT}
    QMAKE_EXTRA_COMPILERS += sse3

    AVX_SOURCES = zimageavx.cpp
    avx.name = avx
    avx.input = AVX_SOURCES
    avx.variable_out = OBJECTS
    avx.output = ${QMAKE_VAR_OBJECTS_DIR}${QMAKE_FILE_IN_BASE}$${first(QMAKE_EXT_OBJ)}
    avx.commands = $${QMAKE_CXX} $(CXXFLAGS) -mavx $(INCPATH) -c ${QMAKE_FILE_IN} -o ${QMAKE_FILE_OUT}
    QMAKE_EXTRA_COMPILERS += avx

    ITKVersion = 4.8
    ITKPath = $$PWD/3rdparty/itk
    INCLUDEPATH += $$ITKPath/include/ITK-$$ITKVersion
    LIBS += -L$$ITKPath/lib -lITKCommon-$$ITKVersion -lITKVNLInstantiation-$$ITKVersion -litkvnl_algo-$$ITKVersion -litkvnl-$$ITKVersion \
      -lITKLabelMap-$$ITKVersion -litkv3p_netlib-$$ITKVersion -litkvcl-$$ITKVersion -litkv3p_lsqr-$$ITKVersion -lITKStatistics-$$ITKVersion \
      -litksys-$$ITKVersion -litkdouble-conversion-$$ITKVersion -litkNetlibSlatec-$$ITKVersion -lITKMetaIO-$$ITKVersion

    JpegPath = $$PWD/3rdparty/libjpeg-turbo
    INCLUDEPATH += $$JpegPath/include
    LIBS += $$JpegPath/lib/libjpeg.a

    JpegXRPath = $$PWD/3rdparty/jxrlib
    LIBS += $$JpegXRPath/lib/libjxrglue.a $$JpegXRPath/lib/libjpegxr.a

    JPGEXR_SOURCES = zimgjpegxr.cpp
    jxr.name = jxr
    jxr.input = JPGEXR_SOURCES
    jxr.variable_out = OBJECTS
    jxr.output = ${QMAKE_VAR_OBJECTS_DIR}${QMAKE_FILE_IN_BASE}$${first(QMAKE_EXT_OBJ)}
    jxr.commands = $${QMAKE_CXX} $(CXXFLAGS) -I$${JpegXRPath}/include/libjxr/common \
                  -I$${JpegXRPath}/include/libjxr/image/x86 -I$${JpegXRPath}/include/libjxr/image \
                  -I$${JpegXRPath}/include/libjxr/glue -D__ANSI__ \
                  -DDISABLE_PERF_MEASUREMENT $(INCPATH) -c ${QMAKE_FILE_IN} -o ${QMAKE_FILE_OUT}
    QMAKE_EXTRA_COMPILERS += jxr

    FreeImagePath = $$PWD/3rdparty/freeimage
    INCLUDEPATH += $$FreeImagePath/include
    LIBS += $$FreeImagePath/lib/libfreeimageplus.dylib

    LIBS += -lz
}

win32 {
    SOURCES += zimagesse3.cpp

    AVX_SOURCES = zimageavx.cpp
    avx.name = avx
    avx.input = AVX_SOURCES
    avx.variable_out = OBJECTS
    avx.output = ${QMAKE_VAR_OBJECTS_DIR}${QMAKE_FILE_IN_BASE}$${first(QMAKE_EXT_OBJ)}
    avx.commands = $${QMAKE_CXX} $(CXXFLAGS) /arch:AVX $(INCPATH) /c ${QMAKE_FILE_IN} /Fo${QMAKE_FILE_OUT}
    QMAKE_EXTRA_COMPILERS += avx

    ITKVersion = 4.8
    ITKPath = "$$PWD\3rdparty\itk"
    INCLUDEPATH += $$ITKPath\include\ITK-$$ITKVersion
    LIBS += -L$$ITKPath\lib -lITKCommon-$$ITKVersion -lITKVNLInstantiation-$$ITKVersion -litkvnl_algo-$$ITKVersion -litkvnl-$$ITKVersion \
      -lITKLabelMap-$$ITKVersion -litkv3p_netlib-$$ITKVersion -litkvcl-$$ITKVersion -litkv3p_lsqr-$$ITKVersion -lITKStatistics-$$ITKVersion \
      -litksys-$$ITKVersion -litkdouble-conversion-$$ITKVersion -litkNetlibSlatec-$$ITKVersion -lITKMetaIO-$$ITKVersion

    JpegPath = "$$PWD\3rdparty\libjpeg-turbo"
    INCLUDEPATH += $$JpegPath\include
    LIBS += $$JpegPath\lib\jpeg-static.lib

    JpegXRPath = $$PWD\3rdparty\jxrlib
    LIBS += $$JpegXRPath\lib\JXRGlueLib.lib $$JpegXRPath\lib\JXRDecodeLib.lib $$JpegXRPath\lib\JXREncodeLib.lib $$JpegXRPath\lib\JXRCommonLib.lib

    JPGEXR_SOURCES = zimgjpegxr.cpp
    jxr.name = jxr
    jxr.input = JPGEXR_SOURCES
    jxr.variable_out = OBJECTS
    jxr.output = ${QMAKE_VAR_OBJECTS_DIR}${QMAKE_FILE_IN_BASE}$${first(QMAKE_EXT_OBJ)}
    jxr.commands = $${QMAKE_CXX} $(CXXFLAGS) /I$${JpegXRPath}\include/libjxr\common \
                  /I$${JpegXRPath}\include\libjxr\image\x86 /I$${JpegXRPath}\include\libjxr\image \
                  /I$${JpegXRPath}\include\libjxr\glue $(INCPATH) /c ${QMAKE_FILE_IN} /Fo${QMAKE_FILE_OUT}
    QMAKE_EXTRA_COMPILERS += jxr

    FreeImagePath = "$$PWD\3rdparty\freeimage"
    INCLUDEPATH += $$FreeImagePath
    LIBS += $$FreeImagePath\FreeImagePlus.lib $$FreeImagePath\FreeImage.lib

    ZlibPath = "$$PWD\3rdparty\zlib"
    INCLUDEPATH += $$ZlibPath\include
    LIBS += $$ZlibPath\lib\zlibstatic.lib

    LIBS += "C:\Program Files (x86)\Intel\Composer XE\compiler\lib\intel64\libiomp5md.lib" \
        "C:\Program Files (x86)\Intel\Composer XE\compiler\lib\intel64\libirc.lib" \
        "odbc32.lib" "odbccp32.lib" "wbemuuid.lib" "d3d9.lib" "kernel32.lib" "user32.lib" "gdi32.lib" \
        "winspool.lib" "comdlg32.lib" "advapi32.lib" "shell32.lib" "ole32.lib" "oleaut32.lib" "uuid.lib"

    RC_FILE = icons/app.rc
}
