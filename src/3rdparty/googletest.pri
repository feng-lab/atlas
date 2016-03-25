INCLUDEPATH += $$PWD/googletest/include $$PWD/googletest

#DEFINES += _USE_GTEST_

SOURCES += $$PWD/googletest/src/gtest-all.cc

unix {
LIBS += -lpthread
}
