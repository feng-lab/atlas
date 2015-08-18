INCLUDEPATH += $$PWD/glbinding/include

DEFINES += GLBINDING_STATIC

win32 {
  LIBS += $$PWD/glbinding/lib/glbinding.lib -lopengl32 -lglu32
}

macx {
  LIBS += $$PWD/glbinding/lib/libglbinding.a -framework AGL -framework OpenGL
}

unix:!macx {
  LIBS += -lGL -lGLU
}
