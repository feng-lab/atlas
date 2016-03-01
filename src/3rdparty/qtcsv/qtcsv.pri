#DEFINES += QTCSV_LIBRARY
INCLUDEPATH += $$PWD

SOURCES += \
    $$PWD/sources/writer.cpp \
    $$PWD/sources/variantdata.cpp \
    $$PWD/sources/stringdata.cpp \
    $$PWD/sources/reader.cpp \
    $$PWD/sources/contentiterator.cpp

HEADERS += \
    $$PWD/include/qtcsv_global.h \
    $$PWD/include/writer.h \
    $$PWD/include/variantdata.h \
    $$PWD/include/stringdata.h \
    $$PWD/include/reader.h \
    $$PWD/include/abstractdata.h \
    $$PWD/sources/filechecker.h \
    $$PWD/sources/contentiterator.h \
    $$PWD/sources/symbols.h
