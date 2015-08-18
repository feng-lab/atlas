INCLUDEPATH += $$PWD

CONFIG += static_libz
CONFIG += static_libjpeg-turbo
CONFIG += static_libtiff

CONFIG += build_atlas

DEFINES += STACK_INTERFACE USE_ITK nullptr=NULL

DEFINES *= QT_USE_QSTRINGBUILDER

HEADERS += \
    $$PWD/zimg.h \
    $$PWD/zimginterface.h \
    $$PWD/zimginfo.h \
    $$PWD/zimgmetatag.h \
    $$PWD/zimgformat.h \
    $$PWD/zimgv3draw.h \
    $$PWD/zimgstackinterface.h \
    $$PWD/zimgio.h \
    $$PWD/zimgtiff.h \
    $$PWD/ztiff.h \
    $$PWD/zimgzeisslsm.h \
    $$PWD/zimgometiff.h \
    $$PWD/zcpuinfo.h \
    $$PWD/zimageinterpolation.h \
    $$PWD/zimage3dutils.h \
    $$PWD/zimage2dutils.h \
    $$PWD/zimgdisplay.h \
    $$PWD/zimg.hxx \
    $$PWD/zstatisticsutils.h \
    $$PWD/zvoxelcoordinate.h \
    $$PWD/zsaturateoperation.h \
    $$PWD/zimgregion.h \
    $$PWD/zbbox.h \
    $$PWD/zexception.h \
    $$PWD/zimgmetadatabase.h \
    $$PWD/zioutils.h \
    $$PWD/zrandom.h \
    $$PWD/zaffine2d.h \
    $$PWD/zaffine3d.h

SOURCES += \
    $$PWD/zimg.cpp \
    $$PWD/zimginfo.cpp \
    $$PWD/zimgmetatag.cpp \
    $$PWD/zimgformat.cpp \
    $$PWD/zimgv3draw.cpp \
    $$PWD/zimginterface.cpp \
    $$PWD/zimgstackinterface.cpp \
    $$PWD/zimgio.cpp \
    $$PWD/zimgtiff.cpp \
    $$PWD/ztiff.cpp \
    $$PWD/zimgzeisslsm.cpp \
    $$PWD/zimgometiff.cpp \
    $$PWD/zcpuinfo.cpp \
    $$PWD/zimageinterpolation.cpp \
    $$PWD/zimgregion.cpp \
    $$PWD/zexception.cpp \
    $$PWD/zioutils.cpp \
    $$PWD/zrandom.cpp \
    $$PWD/zaffine2d.cpp \
    $$PWD/zaffine3d.cpp

exists(/usr/local/include/H5Cpp.h) {
    exists(/usr/local/lib/libhdf5_cpp.a) | exists(/usr/local/lib/libhdf5_cpp.dylib) {
        DEFINES += _USE_HDF5CPP_
        LIBS += -lhdf5_cpp -lhdf5
    }
}

macx {
   LIBS += -L/usr/local/lib -lfftw3 -lfftw3f -lm -framework Accelerate
}

contains(CONFIG, static_libjpeg-turbo) {
    include(3rdparty/libjpeg-turbo.pri)
} else {
    LIBS += -ljpeg
}

contains(CONFIG, static_libz) {
    include($$PWD/3rdparty/zlib.pri)
} else {
    LIBS += -lz
}

contains(CONFIG, static_libtiff) {
    include($$PWD/3rdparty/libtiff.pri)
} else {
    LIBS += -ltiff -llzma
}

