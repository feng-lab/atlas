INCLUDEPATH += $$PWD/googletest/include

#DEFINES += _USE_GTEST_

SOURCES += $$PWD/googletest/src/gtest-all.cc

unix {
LIBS += -lpthread
}
