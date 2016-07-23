INCLUDEPATH += $$PWD/gflags/include
INCLUDEPATH += $$PWD/glog/include

win32 {
  LIBS += $$PWD/glog/lib/libglog.lib $$PWD/gflags/lib/libgflags.lib
}

macx {
  LIBS += $$PWD/glog/lib/libglog.a $$PWD/gflags/lib/libgflags.a
}

unix:!macx {

}
