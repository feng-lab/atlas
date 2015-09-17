#-------------------------------------------------
#
# Project created by QtCreator 2015-06-04T11:15:46
#
#-------------------------------------------------

QT       -= gui

TARGET = img
TEMPLATE = lib
CONFIG += staticlib

macx {
# suppress warnings from 3rd party library, works for gcc and clang
QMAKE_CXXFLAGS += -isystem $$PWD/../3rdparty -isystem $$PWD/../3rdparty/eigen -isystem $$PWD/../3rdparty/boost -isystem $$PWD/../3rdparty/glm \
  -mllvm -inline-threshold=600
}

win32 {
# 0. patch libtiff win32 TIFFFdOpen
# 1. run "build" from 3rdparty folder
# 2. use official qt5

QMAKE_CXXFLAGS += /bigobj /GL # Enables whole program optimization.
QMAKE_LFLAGS += /LTCG # Link-time Code Generation

DEFINES += _CRT_SECURE_NO_WARNINGS NOMINMAX
DEFINES += _USE_MSVC2013_

INCLUDEPATH += $$PWD/../3rdparty $$PWD/../3rdparty/eigen $$PWD/../3rdparty/boost $$PWD/../3rdparty/glm

QMAKE_CXXFLAGS += /wd4267 # 'var' : conversion from 'size_t' to 'type', possible loss of data
QMAKE_CXXFLAGS += /wd4244 # 'argument' : conversion from 'type1' to 'type2', possible loss of data
QMAKE_CXXFLAGS += /wd4305 # 'identifier' : truncation from 'type1' to 'type2'
QMAKE_CXXFLAGS += /wd4819 # The file contains a character that cannot be represented in the current code page (number). Save the file in Unicode format to prevent data loss.

}

#Qt5
isEqual(QT_MAJOR_VERSION,5) | greaterThan(QT_MAJOR_VERSION,5) {
isEqual(QT_MAJOR_VERSION,5) {
  lessThan(QT_MINOR_VERSION,4) {
    message("Cannot build imgio with Qt version $${QT_VERSION}.")
    error("Use at least Qt 5.4.0.")
  }
}
    CONFIG += c++11
}

CONFIG += rtti exceptions

CONFIG += static_libtiff

DEFINES *= QT_USE_QSTRINGBUILDER

HEADERS += \
    zbbox.h \
    zbenchtimer.h \
    zbitset.h \
    zcpuinfo.h \
    zeigenutils.h \
    zexception.h \
    zglobal.h \
    zimage2dutils.h \
    zimage3dutils.h \
    zimageavx.h \
    zimagefilterkernel.h \
    zimagesse3.h \
    zimg.h \
    zimg.hxx \
    zimgformat.h \
    zimgfreeimage.h \
    zimginfo.h \
    zimginterface.h \
    zimgio.h \
    zimgitkimage.h \
    zimgjpeg.h \
    zimgjpegxr.h \
    zimgmetadatabase.h \
    zimgmetaimage.h \
    zimgmetatag.h \
    zimgometiff.h \
    zimgpng.h \
    zimgregion.h \
    zimgsliceprovider.h \
    zimgstackinterface.h \
    zimgtiff.h \
    zimgv3draw.h \
    zimgzeissczi.h \
    zimgzeisslsm.h \
    zioutils.h \
    zrandom.h \
    zsaturateoperation.h \
    zstatisticsutils.h \
    zstringutils.h \
    ztiff.h \
    zvoxelcoordinate.h \
    zimgregioniterator.h \
    zimgneighborhooditerator.h \
    zimgneighborhoodwithcoorditerator.h \
    zneighborhood.h \
    zimgneighborhoodwithptriterator.h \
    $$PWD/../3rdparty/lodepng/lodepng.h

SOURCES += \
    zbenchtimer.cpp \
    zcpuinfo.cpp \
    zeigenutils.cpp \
    zexception.cpp \
    zimage2dutils.cpp \
    zimage3dutils.cpp \
    zimagefilterkernel.cpp \
    zimg.cpp \
    zimgformat.cpp \
    zimgfreeimage.cpp \
    zimginfo.cpp \
    zimginterface.cpp \
    zimgio.cpp \
    zimgitkimage.cpp \
    zimgjpeg.cpp \
    zimgmetaimage.cpp \
    zimgmetatag.cpp \
    zimgometiff.cpp \
    zimgpng.cpp \
    zimgregion.cpp \
    zimgsliceprovider.cpp \
    zimgstackinterface.cpp \
    zimgtiff.cpp \
    zimgv3draw.cpp \
    zimgzeissczi.cpp \
    zimgzeisslsm.cpp \
    zioutils.cpp \
    zrandom.cpp \
    zstringutils.cpp \
    ztiff.cpp \
    zneighborhood.cpp \
    $$PWD/../3rdparty/lodepng/lodepng.cpp

contains(CONFIG, static_libtiff) {
    include($$PWD/../3rdparty/libtiff.pri)
} else {
    LIBS += -ltiff -llzma
}

include($$PWD/../3rdparty/QsLog/QsLog.pri)

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

exists(/opt/intel/tbb/include) {
    TBBPath = /opt/intel/tbb
    INCLUDEPATH += $$TBBPath/include
    LIBS += $$TBBPath/lib/libtbb.dylib
} else {
    DEFINES += _USE_QTCONCURRENT_
    QT += concurrent
}

    ITKVersion = 4.9
    ITKPath = $$PWD/../3rdparty/itk
    INCLUDEPATH += $$ITKPath/include/ITK-$$ITKVersion
    LIBS += -L$$ITKPath/lib -lITKCommon-$$ITKVersion -lITKVNLInstantiation-$$ITKVersion -litkvnl_algo-$$ITKVersion -litkvnl-$$ITKVersion \
      -lITKLabelMap-$$ITKVersion -litkv3p_netlib-$$ITKVersion -litkvcl-$$ITKVersion -litkv3p_lsqr-$$ITKVersion -lITKStatistics-$$ITKVersion \
      -litksys-$$ITKVersion -litkdouble-conversion-$$ITKVersion -litkNetlibSlatec-$$ITKVersion -lITKMetaIO-$$ITKVersion \
      -lITKIOImageBase-$$ITKVersion -lITKIONIFTI-$$ITKVersion -lITKniftiio-$$ITKVersion -lITKznz-$$ITKVersion \
      -lITKIONRRD-$$ITKVersion -lITKNrrdIO-$$ITKVersion \
      #-lITKIOGDCM-$$ITKVersion -litkgdcmDICT-$$ITKVersion -litkgdcmMSFF-$$ITKVersion -litkgdcmCommon-$$ITKVersion \
      #-litkgdcmIOD-$$ITKVersion -litkgdcmDSED-$$ITKVersion -litkgdcmjpeg8-$$ITKVersion -litkgdcmjpeg12-$$ITKVersion -litkgdcmjpeg16-$$ITKVersion \
      #-litkgdcmuuid-$$ITKVersion -litkopenjpeg-$$ITKVersion -lITKEXPAT-$$ITKVersion

    JpegPath = $$PWD/../3rdparty/libjpeg-turbo
    INCLUDEPATH += $$JpegPath/include
    LIBS += $$JpegPath/lib/libjpeg.a

    JpegXRPath = $$PWD/../3rdparty/jxrlib
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

    FreeImagePath = $$PWD/../3rdparty/freeimage
    INCLUDEPATH += $$FreeImagePath/include
    LIBS += $$FreeImagePath/lib/libfreeimageplus.dylib
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

    TBBPath = "C:\Program Files (x86)\Intel\Composer XE\tbb"
    INCLUDEPATH += $$TBBPath\include
    LIBS += $$TBBPath\lib\intel64\vc12\tbb.lib
    QMAKE_POST_LINK += robocopy /is \"$$TBBPath\..\redist\intel64\tbb\vc12\" $$OUT_PWD/release tbb.dll &

    ITKVersion = 4.9
    ITKPath = "$$PWD\..\3rdparty\itk"
    INCLUDEPATH += $$ITKPath\include\ITK-$$ITKVersion
    LIBS += -L$$ITKPath\lib -lITKCommon-$$ITKVersion -lITKVNLInstantiation-$$ITKVersion -litkvnl_algo-$$ITKVersion -litkvnl-$$ITKVersion \
      -lITKLabelMap-$$ITKVersion -litkv3p_netlib-$$ITKVersion -litkvcl-$$ITKVersion -litkv3p_lsqr-$$ITKVersion -lITKStatistics-$$ITKVersion \
      -litksys-$$ITKVersion -litkdouble-conversion-$$ITKVersion -litkNetlibSlatec-$$ITKVersion -lITKMetaIO-$$ITKVersion \
      -lITKIOImageBase-$$ITKVersion -lITKIONIFTI-$$ITKVersion -lITKniftiio-$$ITKVersion -lITKznz-$$ITKVersion \
      -lITKIONRRD-$$ITKVersion -lITKNrrdIO-$$ITKVersion \
      #-lITKIOGDCM-$$ITKVersion -litkgdcmDICT-$$ITKVersion -litkgdcmMSFF-$$ITKVersion -litkgdcmCommon-$$ITKVersion \
      #-litkgdcmIOD-$$ITKVersion -litkgdcmDSED-$$ITKVersion -litkgdcmjpeg8-$$ITKVersion -litkgdcmjpeg12-$$ITKVersion -litkgdcmjpeg16-$$ITKVersion \
      #-litkgdcmuuid-$$ITKVersion -litkopenjpeg-$$ITKVersion -lITKEXPAT-$$ITKVersion

    JpegPath = "$$PWD\..\3rdparty\libjpeg-turbo"
    INCLUDEPATH += $$JpegPath\include
    LIBS += $$JpegPath\lib\jpeg-static.lib

    JpegXRPath = $$PWD\..\3rdparty\jxrlib
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

    FreeImagePath = "$$PWD\..\3rdparty\freeimage"
    INCLUDEPATH += $$FreeImagePath
    LIBS += $$FreeImagePath\FreeImagePlus.lib $$FreeImagePath\FreeImage.lib
    QMAKE_POST_LINK += robocopy /is $$FreeImagePath $$OUT_PWD/release *.dll &

    ZlibPath = "$$PWD\..\3rdparty\zlib"
    INCLUDEPATH += $$ZlibPath\include
    LIBS += $$ZlibPath\lib\zlibstatic.lib

    LIBS += "C:\Program Files (x86)\Intel\Composer XE\compiler\lib\intel64\libiomp5md.lib" \
        "C:\Program Files (x86)\Intel\Composer XE\compiler\lib\intel64\libirc.lib"
#        "odbc32.lib" "odbccp32.lib" "wbemuuid.lib" "d3d9.lib" "kernel32.lib" "user32.lib" "gdi32.lib" \
#        "winspool.lib" "comdlg32.lib" "advapi32.lib" "shell32.lib" "ole32.lib" "oleaut32.lib" "uuid.lib"
}
