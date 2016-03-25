TEMPLATE = app

CONFIG(debug, debug|release) {
    TARGET = atlas_d
} else {
    TARGET = atlas
}

macx {
# suppress warnings from 3rd party library, works for gcc and clang
QMAKE_CXXFLAGS += -isystem $$PWD/../3rdparty -isystem $$PWD/../3rdparty/eigen -isystem $$PWD/../3rdparty/boost -isystem $$PWD/../3rdparty/glm \
  -mllvm -inline-threshold=600 #--analyze
QMAKE_LFLAGS += -rpath @executable_path/../Frameworks
}

win32 {
QMAKE_CXXFLAGS += /bigobj /GL # Enables whole program optimization.
QMAKE_LFLAGS += /LTCG # Link-time Code Generation

DEFINES += _CRT_SECURE_NO_WARNINGS NOMINMAX
#DEFINES += _USE_MSVC2013_

INCLUDEPATH += $$PWD/../3rdparty $$PWD/3rdparty/../eigen $$PWD/../3rdparty/boost $$PWD/../3rdparty/glm

QMAKE_CXXFLAGS += /wd4267 # 'var' : conversion from 'size_t' to 'type', possible loss of data
QMAKE_CXXFLAGS += /wd4244 # 'argument' : conversion from 'type1' to 'type2', possible loss of data
QMAKE_CXXFLAGS += /wd4305 # 'identifier' : truncation from 'type1' to 'type2'
QMAKE_CXXFLAGS += /wd4819 # The file contains a character that cannot be represented in the current code page (number). Save the file in Unicode format to prevent data loss.
}

QT += opengl network svg

#Qt5
isEqual(QT_MAJOR_VERSION,5) | greaterThan(QT_MAJOR_VERSION,5) {
isEqual(QT_MAJOR_VERSION,5) {
  lessThan(QT_MINOR_VERSION,4) {
    message("Cannot build Atlas with Qt version $${QT_VERSION}.")
    error("Use at least Qt 5.4.0.")
  }
}
    QT += gui
    QMAKE_CXXFLAGS += -mmacosx-version-min=10.7 -std=c++14 -stdlib=libc++
    QMAKE_LFLAGS += -stdlib=libc++
}

#Qt4
isEqual(QT_MAJOR_VERSION,4) {
    message("Qt 4")
    include($$PWD/../3rdparty/qjson/qjson.pri)
    DEFINES += _QT4_
    QMAKE_CXXFLAGS += -mmacosx-version-min=10.7 -std=c++14 -stdlib=libc++
    QMAKE_LFLAGS += -stdlib=libc++
}

CONFIG += rtti exceptions

CONFIG += use_glbinding
CONFIG += with_tests

DEFINES *= QT_USE_QSTRINGBUILDER
DEFINES += _USE_CORE_PROFILE_

RESOURCES = atlas.qrc
HEADERS += \
    zimageinterpolation.h \
    zimgdisplay.h \
    zimgmerge.h \
    zimgtile.h \
    zvoxelregion.h \
    zimgncc.h \
    zcompleximg.h \
    zfft.h \
    zimagetoimagemetric.h \
    zregistrationoptimizer.h \
    zregistrationcostfunction.h \
    zsectionsregistrationdialog.h \
    zsectionsregistration.h \
    zstitchimagedialog.h \
    zpunctadetectiondialog.h \
    zpunctadetection.h \
    zassignpuncta.h \
    zgenerateanalysistextfile.h \
    zanalysisworklistdialog.h \
    zanalysisworklistmodel.h \
    zstyleditemdelegate.h \
    zitemeditorfactory.h \
    zimgalgorithm.h \
    zimgconnectedcomponents.h \
    zimgregionalextrema.h \
    zimgautothreshold.h \
    zimgitkinterface.h \
    zimgprocess.h \
    zimgsigneddistancemap.h \
    zimggraph.h \
    zimgnccmatch.h \
    zmainwindow.h \
    zimgdoc.h \
    zimgview.h \
    zspinboxwithscrollbar.h \
    zgraphicsview.h \
    zview.h \
    zdoc.h \
    zobjdoc.h \
    zobjview.h \
    zimgfilter.h \
    zobjmodel.h \
    zobjwidget.h \
    zshareitemselectionmodel.h \
    zitemselectionmodel.h \
    zviewsettingwidget.h \
    z3dmainwindow.h \
    z3dview.h \
    z3dobjview.h \
    z3dimgview.h \
    zviewsettinginterface.h \
    zpunctadoc.h \
    z3dpunctaview.h \
    z3dpunctafilter.h \
    z3dmeshfilter.h \
    z3dswcfilter.h \
    zmeshdoc.h \
    zswcdoc.h \
    z3dswcview.h \
    z3dmeshview.h \
    zobjfilter.h \
    zanimation.h \
    zparameteranimation.h \
    zparameterkey.h \
    zcameraparameterkey.h \
    zcameraparameteranimation.h \
    z3danimation.h \
    z3danimationdoc.h \
    zsaveobjsdialog.h \
    zanimationwidget.h \
    zobjeditwidget.h \
    zparameterfactory.h \
    zfontparameter.h \
    zfontwidget.h \
    z2danimation.h \
    z2danimationdoc.h \
    ztimelinewidget.h \
    ztimelineobjview.h \
    ztimelineeventview.h \
    ztimelineobjscene.h \
    ztimelineeventscene.h \
    ztimelineaxisview.h \
    ztimelinekeyeditdialog.h \
    zdockwidget.h \
    z3danimationview.h \
    z3danimationfilter.h \
    z3dcompositor.h \
    z3dgeometryfilter.h \
    z3daxisfilter.h \
    z3dboundedfilter.h \
    z3dvolumeslicerenderer.h \
    z3dmeshrenderer.h \
    z3dtexturecopyrenderer.h \
    z3dtexturecoordinaterenderer.h \
    z3dtextureblendrenderer.h \
    z3dsphererenderer.h \
    z3dlinewithfixedwidthcolorrenderer.h \
    z3dlinerenderer.h \
    z3dimage2drenderer.h \
    z3dfontrenderer.h \
    z3dellipsoidrenderer.h \
    z3dconerenderer.h \
    z3dbackgroundrenderer.h \
    z3darrowrenderer.h \
    z3drendererbase.h \
    z3dprimitiverenderer.h \
    z3dcanvaspainter.h \
    z3dnetworkevaluator.h \
    z3dcanvas.h \
    z3dscene.h \
    z3dshadergroup.h \
    z3dport.h \
    z3drenderport.h \
    z3dglobalparameters.h \
    z3dshaderprogram.h \
    z3dvolume.h \
    z3dtransferfunction.h \
    z3dtransferfunctioneditor.h \
    z3dtransferfunctionwidgetwitheditorwindow.h \
    zclickablelabel.h \
    zcolormap.h \
    zcolormapeditor.h \
    zcolormapwidgetwitheditorwindow.h \
    zvertexarrayobject.h \
    zvertexbufferobject.h \
    z3dpickingmanager.h \
    z3dfilterview.h \
    zroi.h \
    zroidoc.h \
    zfilterview.h \
    zroiview.h \
    zroifilter.h \
    zactiongroup.h \
    zboostgeometryadaptor.h \
    zquatparameter.h \
    z3dshadermanager.h \
    zanimationexportwidget.h \
    zvideoencoder.h \
    zmesh.h \
    zmeshio.h \
    zmeshutils.h \
    zpuncta.h \
    zpunctaio.h \
    zpunctum.h \
    zgmm.h \
    zkmeans.h \
    zvbgmm.h \
    zswcfilter.h \
    zswcview.h \
    zpunctafilter.h \
    zpunctaview.h \
    zswc.h \
    ztree.hpp \
    znumericparameter.h \
    zoptionparameter.h \
    zparameter.h \
    zglmutils.h \
    zcombobox.h \
    zselectfilewidget.h \
    zloadimagesequencedialog.h \
    zspinbox.h \
    zspinboxwithslider.h \
    zwidgetsgroup.h \
    z3dcamera.h \
    z3dcameraparameter.h \
    z3drendertarget.h \
    z3dinteractionhandler.h \
    zeventlistenerparameter.h \
    z3dcanvaseventlistener.h \
    z3dtexture.h \
    z3dtransformparameter.h \
    z3dgpuinfo.h \
    zstringparameter.h \
    z3dsdfont.h \
    zspanslider.h \
    zcustomcommand.h \
    zaffine2d.h \
    zaffine3d.h \
    ztakescreenshotwidget.h \
    zimgregistration.h \
    zimagetransform.h \
    zimagematrix2dtransform.h \
    zimagematrix3dtransform.h \
    zregistrationnumericdiffcostfunction.h \
    zimagecompositetransform.h \
    zimagetransformresolve.h \
    z3dshader.h \
    z3dcontext.h \
    z3dgl.h \
    zimgpack.h \
    zimgpackdisplay.h \
    zimgvoxelcolormap.h \
    zimgcache.h \
    zsysteminfo.h \
    zfileutils.h \
    zregionannotation.h \
    zregionannotationdoc.h \
    z3dregionannotationview.h \
    zregionannotationview.h \
    zregionannotationfilter.h \
    z3dregionannotationfilter.h \
    zimgfillhole.h \
    zregionannotationwidget.h \
    zregionannotationtreemodel.h \
    zregionannotationtreeview.h \
    zbuttoncolumndelegate.h \
    zchooseobjdialog.h \
    zgraphicsscene.h \
    z3dfilter.h \
    z3dtextureglowrenderer.h \
    z3dcameracontrolwidget.h \
    z3dtextureandeyecoordinaterenderer.h \
    z3dimgraycasterrenderer.h \
    z3dimg.h \
    z3dimgfilter.h \
    z3dblockcache.h \
    z3dimgslicerenderer.h \
    zregionontology.h \
    zobjdetailedinfowidget.h \
    zjson.h

SOURCES += \
    main.cpp \
    zimageinterpolation.cpp \
    zimgdisplay.cpp \
    zimgmerge.cpp \
    zimgtile.cpp \
    zvoxelregion.cpp \
    zimgncc.cpp \
    zcompleximg.cpp \
    zfft.cpp \
    zimagetoimagemetric.cpp \
    zregistrationoptimizer.cpp \
    zregistrationcostfunction.cpp \
    zsectionsregistrationdialog.cpp \
    zsectionsregistration.cpp \
    zstitchimagedialog.cpp \
    zpunctadetectiondialog.cpp \
    zpunctadetection.cpp \
    zassignpuncta.cpp \
    zgenerateanalysistextfile.cpp \
    zanalysisworklistdialog.cpp \
    zanalysisworklistmodel.cpp \
    zstyleditemdelegate.cpp \
    zitemeditorfactory.cpp \
    zimgalgorithm.cpp \
    zimgprocess.cpp \
    zimggraph.cpp \
    zimgnccmatch.cpp \
    zimgsigneddistancemap.cpp \
    zimgconnectedcomponents.cpp \
    zimgautothreshold.cpp \
    zimgregionalextrema.cpp \
    zmainwindow.cpp \
    zimgdoc.cpp \
    zimgview.cpp \
    zspinboxwithscrollbar.cpp \
    zgraphicsview.cpp \
    zview.cpp \
    zdoc.cpp \
    zobjdoc.cpp \
    zobjview.cpp \
    zimgfilter.cpp \
    zobjmodel.cpp \
    zobjwidget.cpp \
    zshareitemselectionmodel.cpp \
    zitemselectionmodel.cpp \
    zviewsettingwidget.cpp \
    z3dmainwindow.cpp \
    z3dview.cpp \
    z3dobjview.cpp \
    z3dimgview.cpp \
    zpunctadoc.cpp \
    z3dpunctaview.cpp \
    z3dpunctafilter.cpp \
    z3dmeshfilter.cpp \
    z3dswcfilter.cpp \
    zmeshdoc.cpp \
    zswcdoc.cpp \
    z3dswcview.cpp \
    z3dmeshview.cpp \
    zobjfilter.cpp \
    zanimation.cpp \
    zparameteranimation.cpp \
    zparameterkey.cpp \
    zcameraparameterkey.cpp \
    zcameraparameteranimation.cpp \
    z3danimation.cpp \
    z3danimationdoc.cpp \
    zsaveobjsdialog.cpp \
    zanimationwidget.cpp \
    zobjeditwidget.cpp \
    zparameterfactory.cpp \
    zfontparameter.cpp \
    zfontwidget.cpp \
    z2danimation.cpp \
    z2danimationdoc.cpp \
    ztimelinewidget.cpp \
    ztimelineobjview.cpp \
    ztimelineeventview.cpp \
    ztimelineobjscene.cpp \
    ztimelineeventscene.cpp \
    ztimelineaxisview.cpp \
    ztimelinekeyeditdialog.cpp \
    zdockwidget.cpp \
    z3danimationview.cpp \
    z3danimationfilter.cpp \
    z3dcompositor.cpp \
    z3dgeometryfilter.cpp \
    z3daxisfilter.cpp \
    z3dboundedfilter.cpp \
    z3dvolumeslicerenderer.cpp \
    z3dmeshrenderer.cpp \
    z3dtexturecopyrenderer.cpp \
    z3dtexturecoordinaterenderer.cpp \
    z3dtextureblendrenderer.cpp \
    z3dsphererenderer.cpp \
    z3dprimitiverenderer.cpp \
    z3dlinewithfixedwidthcolorrenderer.cpp \
    z3dlinerenderer.cpp \
    z3dimage2drenderer.cpp \
    z3dfontrenderer.cpp \
    z3dellipsoidrenderer.cpp \
    z3dconerenderer.cpp \
    z3dbackgroundrenderer.cpp \
    z3darrowrenderer.cpp \
    z3drendererbase.cpp \
    z3dcanvaspainter.cpp \
    z3dnetworkevaluator.cpp \
    z3dcanvas.cpp \
    z3dscene.cpp \
    z3dshadergroup.cpp \
    z3dport.cpp \
    z3drenderport.cpp \
    z3dglobalparameters.cpp \
    z3dshaderprogram.cpp \
    z3dvolume.cpp \
    z3dtransferfunction.cpp \
    z3dtransferfunctioneditor.cpp \
    z3dtransferfunctionwidgetwitheditorwindow.cpp \
    zclickablelabel.cpp \
    zcolormap.cpp \
    zcolormapeditor.cpp \
    zcolormapwidgetwitheditorwindow.cpp \
    zvertexarrayobject.cpp \
    zvertexbufferobject.cpp \
    z3dpickingmanager.cpp \
    zroi.cpp \
    zroidoc.cpp \
    zroiview.cpp \
    zroifilter.cpp \
    zactiongroup.cpp \
    zquatparameter.cpp \
    z3dshadermanager.cpp \
    zanimationexportwidget.cpp \
    zvideoencoder.cpp \
    zmesh.cpp \
    zmeshio.cpp \
    zmeshutils.cpp \
    zpuncta.cpp \
    zpunctaio.cpp \
    zpunctum.cpp \
    zswcfilter.cpp \
    zswcview.cpp \
    zpunctafilter.cpp \
    zpunctaview.cpp \
    zswc.cpp \
    znumericparameter.cpp \
    zoptionparameter.cpp \
    zparameter.cpp \
    zcombobox.cpp \
    zselectfilewidget.cpp \
    zloadimagesequencedialog.cpp \
    zspinbox.cpp \
    zspinboxwithslider.cpp \
    zwidgetsgroup.cpp \
    z3dcamera.cpp \
    z3dcameraparameter.cpp \
    z3drendertarget.cpp \
    z3dinteractionhandler.cpp \
    zeventlistenerparameter.cpp \
    z3dtexture.cpp \
    z3dtransformparameter.cpp \
    z3dgpuinfo.cpp \
    zstringparameter.cpp \
    z3dsdfont.cpp \
    zspanslider.cpp \
    zcustomcommand.cpp \
    zaffine2d.cpp \
    zaffine3d.cpp \
    ztakescreenshotwidget.cpp \
    zimgregistration.cpp \
    zimagetransform.cpp \
    zimagematrix2dtransform.cpp \
    zimagematrix3dtransform.cpp \
    zregistrationnumericdiffcostfunction.cpp \
    zimagecompositetransform.cpp \
    zimagetransformresolve.cpp \
    z3dshader.cpp \
    z3dcontext.cpp \
    z3dgl.cpp \
    zimgpack.cpp \
    zimgpackdisplay.cpp \
    zimgcache.cpp \
    zsysteminfo.cpp \
    zfileutils.cpp \
    zregionannotation.cpp \
    zregionannotationdoc.cpp \
    z3dregionannotationview.cpp \
    zregionannotationview.cpp \
    zregionannotationfilter.cpp \
    z3dregionannotationfilter.cpp \
    zimgfillhole.cpp \
    zregionannotationwidget.cpp \
    zregionannotationtreemodel.cpp \
    zregionannotationtreeview.cpp \
    zbuttoncolumndelegate.cpp \
    zchooseobjdialog.cpp \
    zgraphicsscene.cpp \
    z3dfilter.cpp \
    z3dtextureglowrenderer.cpp \
    z3dcameracontrolwidget.cpp \
    z3dtextureandeyecoordinaterenderer.cpp \
    z3dimgraycasterrenderer.cpp \
    z3dimg.cpp \
    z3dimgfilter.cpp \
    z3dimgslicerenderer.cpp \
    zregionontology.cpp \
    zobjdetailedinfowidget.cpp \
    zjson.cpp

contains(CONFIG, use_glbinding) {
    include($$PWD/../3rdparty/glbinding.pri)
} else {
    DEFINES += _USE_GLEW_
    include($$PWD/../3rdparty/glew.pri)
}

contains(CONFIG, with_tests) {
    DEFINES += _WITH_TESTS_
    include($$PWD/../3rdparty/googletest.pri)

    HEADERS += \
      zunittest.h \
      test/zimggraphtest.h \
      test/zimagetoimagemetrictest.h \
      test/zimageutilstest.h \
      test/zimageaffinetransformtest.h \
      test/zimgncctest.h \
      test/zfilereadtest.h \
      test/zimgiteratortest.h \
      test/zsaturateoperationtest.h \
      test/zclustertest.h \
      test/zimgtest.h \
      test/zimgconnectedcomponentstest.h \
      test/zimgsigneddistancemaptest.h \
      test/zimgautothresholdtest.h \
      test/zimgregionalextrematest.h

    SOURCES += zunittest.cpp
}

#include($$PWD/../3rdparty/qwt/qwt.pri)
include($$PWD/../3rdparty/optimization/optimization.pri)
include($$PWD/../3rdparty/libqxt.pri)
include($$PWD/../3rdparty/QsLog/QsLogGUI.pri)
include($$PWD/../3rdparty/eigenGUI.pri)
include($$PWD/../3rdparty/qtcsv/qtcsv.pri)

macx {
    SOURCES += $$PWD/../3rdparty/sys/VideoMemoryMac.cpp

exists(/opt/intel/mkl/include) {
    DEFINES += _USE_MKL_
    MKLPath = /opt/intel/mkl
    INCLUDEPATH += $$MKLPath/include $$MKLPath/include/fftw
    LIBS += $$MKLPath/lib/libmkl_intel_lp64.a $$MKLPath/lib/libmkl_intel_thread.a $$MKLPath/lib/libmkl_core.a \
            -L/opt/intel/lib -liomp5 -lpthread -lm
    QMAKE_POST_LINK += install_name_tool -change libiomp5.dylib @rpath/libiomp5.dylib $$OUT_PWD/$${TARGET}.app/Contents/MacOS/$${TARGET} $$escape_expand(\\n\\t)
    MKL.files = /opt/intel/lib/libiomp5.dylib
    MKL.path = Contents/Frameworks
    QMAKE_BUNDLE_DATA += MKL
} else {
    INCLUDEPATH += /usr/local/include
    LIBS += -L/usr/local/lib -lfftw3_threads -lfftw3 -lfftw3f -lm -framework Accelerate
}

exists(/opt/intel/ipp/include) {
    DEFINES += _USE_IPP_
    IPPPath = /opt/intel/ipp
    INCLUDEPATH += $$IPPPath/include
    LIBS += $$IPPPath/lib/libippi.a $$IPPPath/lib/libippcore.a $$IPPPath/lib/libippvm.a $$IPPPath/lib/libipps.a $$IPPPath/lib/libippcv.a $$IPPPath/lib/libippcc.a \
      /opt/intel/lib/libirc.a
}

exists(/opt/intel/tbb/include) {
    TBBPath = /opt/intel/tbb
    INCLUDEPATH += $$TBBPath/include
    LIBS += $$TBBPath/lib/libtbb.dylib
    TBB.files = $$TBBPath/lib/libtbb.dylib
    TBB.path = Contents/Frameworks
    QMAKE_BUNDLE_DATA += TBB
} else {
    DEFINES += _USE_QTCONCURRENT_
    QT += concurrent
}

    ITKVersion = 4.10
    ITKPath = $$PWD/../3rdparty/itk
    INCLUDEPATH += $$ITKPath/include/ITK-$$ITKVersion $$ITKPath/include/vxl/vcl $$ITKPath/include/vxl/core
    LIBS += -L$$ITKPath/lib -lITKCommon-$$ITKVersion -lITKVNLInstantiation-$$ITKVersion -litkvnl_algo-$$ITKVersion -litkvnl-$$ITKVersion \
      -lITKLabelMap-$$ITKVersion -litkv3p_netlib-$$ITKVersion -litkvcl-$$ITKVersion -lITKStatistics-$$ITKVersion \
      -litksys-$$ITKVersion -litkdouble-conversion-$$ITKVersion -litkNetlibSlatec-$$ITKVersion -lITKMetaIO-$$ITKVersion \
      -lITKIOImageBase-$$ITKVersion -lITKIONIFTI-$$ITKVersion -lITKniftiio-$$ITKVersion -lITKznz-$$ITKVersion \
      -lITKIONRRD-$$ITKVersion -lITKNrrdIO-$$ITKVersion \
      #-lITKIOGDCM-$$ITKVersion -litkgdcmDICT-$$ITKVersion -litkgdcmMSFF-$$ITKVersion -litkgdcmCommon-$$ITKVersion \
      #-litkgdcmIOD-$$ITKVersion -litkgdcmDSED-$$ITKVersion -litkgdcmjpeg8-$$ITKVersion -litkgdcmjpeg12-$$ITKVersion -litkgdcmjpeg16-$$ITKVersion \
      #-litkgdcmuuid-$$ITKVersion -litkopenjpeg-$$ITKVersion -lITKEXPAT-$$ITKVersion

    VTKVersion = 7.1
    VTKPath = $$PWD/../3rdparty/vtk
    INCLUDEPATH += $$VTKPath/include/vtk-$$VTKVersion
    LIBS += -L$$VTKPath/lib -lvtkFiltersGeometry-$$VTKVersion -lvtkFiltersGeneral-$$VTKVersion -lvtkCommonComputationalGeometry-$$VTKVersion \
      -lvtkFiltersSources-$$VTKVersion -lvtkFiltersCore-$$VTKVersion -lvtkCommonExecutionModel-$$VTKVersion -lvtkCommonDataModel-$$VTKVersion \
      -lvtkCommonSystem-$$VTKVersion -lvtkCommonMisc-$$VTKVersion -lvtkCommonTransforms-$$VTKVersion \
      -lvtkCommonMath-$$VTKVersion -lvtkCommonCore-$$VTKVersion -lvtksys-$$VTKVersion

    WildMagicSDKPath = $$PWD/../3rdparty/wildmagic
    INCLUDEPATH += $$WildMagicSDKPath/include
    LIBS += $$WildMagicSDKPath/lib/libGTEngine.a

    JpegPath = $$PWD/../3rdparty/libjpeg-turbo
    INCLUDEPATH += $$JpegPath/include
    LIBS += $$JpegPath/lib/libjpeg.a

    JpegXRPath = $$PWD/../3rdparty/jxrlib
    LIBS += $$JpegXRPath/lib/libjxrglue.a $$JpegXRPath/lib/libjpegxr.a

    FreeImagePath = $$PWD/../3rdparty/freeimage
    INCLUDEPATH += $$FreeImagePath/include
    LIBS += $$FreeImagePath/lib/libfreeimageplus.dylib

    HDF5Path = $$PWD/../3rdparty/hdf5
    INCLUDEPATH += $$HDF5Path/include
    LIBS += $$HDF5Path/lib/libhdf5_cpp.a $$HDF5Path/lib/libhdf5.a

    AssimpPath = $$PWD/../3rdparty/assimp
    INCLUDEPATH += $$AssimpPath/include
    LIBS += $$AssimpPath/lib/libassimp.dylib

#    CGALPath = /usr/local
#    DEFINES += CGAL_NDEBUG
#    INCLUDEPATH += /usr/local/Cellar/cgal/4.6/include /usr/local/Cellar/gmp/6.0.0a/include /usr/local/Cellar/mpfr/3.1.2-p11/include
#    LIBS += $$CGALPath/lib/libCGAL_ImageIO.dylib $$CGALPath/lib/libCGAL_Core.dylib $$CGALPath/lib/libCGAL.dylib $$CGALPath/lib/libgmp.dylib $$CGALPath/lib/libmpfr.dylib \
#        $$CGALPath/lib/libboost_thread-mt.dylib $$CGALPath/lib/libboost_system-mt.dylib

    OpencvPath = $$PWD/../3rdparty/opencv
    INCLUDEPATH += $$OpencvPath/include
    LIBS += $$OpencvPath/lib/libopencv_imgproc.a $$OpencvPath/lib/libopencv_core.a

    ffmpeg.files = $$PWD/../../ffmpeg
    ffmpeg.path = Contents/Resources
    QMAKE_BUNDLE_DATA += ffmpeg

    LIBS += -lz -framework AppKit -framework IOKit \
        -framework ApplicationServices \
        -framework CoreFoundation -framework OpenCL

    ICON = icons/app.icns
    QMAKE_INFO_PLIST = Resources/Info.plist
}

win32 {
    SOURCES += 3rdparty/sys/VideoMemoryWin.cpp \
        3rdparty/sys/VidMemViaD3D9.cpp \
        3rdparty/sys/VidMemViaDDraw.cpp \
        3rdparty/sys/VidMemViaDxDiag.cpp

    DEFINES += _USE_MKL_
    MKLPath = "C:\Program Files (x86)\Intel\Composer XE\mkl"
    INCLUDEPATH += $$MKLPath\include $$MKLPath\include\fftw
    LIBS += $$MKLPath\lib\intel64\mkl_intel_lp64.lib $$MKLPath\lib\intel64\mkl_intel_thread.lib $$MKLPath\lib\intel64\mkl_core.lib

    DEFINES += _USE_IPP_
    IPPPath = "C:\Program Files (x86)\Intel\Composer XE\ipp"
    INCLUDEPATH += $$IPPPath\include
    LIBS += -L$$IPPPath\lib\intel64 ippimt.lib ippcoremt.lib ippvmmt.lib ippsmt.lib ippcvmt.lib ippccmt.lib

    TBBPath = "C:\Program Files (x86)\Intel\Composer XE\tbb"
    INCLUDEPATH += $$TBBPath\include
    LIBS += $$TBBPath\lib\intel64\vc12\tbb.lib
    QMAKE_POST_LINK += robocopy /is \"$$TBBPath\..\redist\intel64\tbb\vc12\" $$OUT_PWD/release tbb.dll &

    ITKVersion = 4.10
    ITKPath = "$$PWD\..\3rdparty\itk"
    INCLUDEPATH += $$ITKPath\include\ITK-$$ITKVersion
    LIBS += -L$$ITKPath\lib -lITKCommon-$$ITKVersion -lITKVNLInstantiation-$$ITKVersion -litkvnl_algo-$$ITKVersion -litkvnl-$$ITKVersion \
      -lITKLabelMap-$$ITKVersion -litkv3p_netlib-$$ITKVersion -litkvcl-$$ITKVersion -lITKStatistics-$$ITKVersion \
      -litksys-$$ITKVersion -litkdouble-conversion-$$ITKVersion -litkNetlibSlatec-$$ITKVersion -lITKMetaIO-$$ITKVersion \
      -lITKIOImageBase-$$ITKVersion -lITKIONIFTI-$$ITKVersion -lITKniftiio-$$ITKVersion -lITKznz-$$ITKVersion \
      -lITKIONRRD-$$ITKVersion -lITKNrrdIO-$$ITKVersion \
      #-lITKIOGDCM-$$ITKVersion -litkgdcmDICT-$$ITKVersion -litkgdcmMSFF-$$ITKVersion -litkgdcmCommon-$$ITKVersion \
      #-litkgdcmIOD-$$ITKVersion -litkgdcmDSED-$$ITKVersion -litkgdcmjpeg8-$$ITKVersion -litkgdcmjpeg12-$$ITKVersion -litkgdcmjpeg16-$$ITKVersion \
      #-litkgdcmuuid-$$ITKVersion -litkopenjpeg-$$ITKVersion -lITKEXPAT-$$ITKVersion

    WildMagicSDKPath = "$$PWD\..\3rdparty\wildmagic"
    INCLUDEPATH += $$WildMagicSDKPath\Include
    LIBS += $$WildMagicSDKPath\Library\v120\x64\Release\Wm5Mathematics.lib $$WildMagicSDKPath\Library\v120\x64\Release\Wm5Core.lib

    JpegPath = "$$PWD\..\3rdparty\libjpeg-turbo"
    INCLUDEPATH += $$JpegPath\include
    LIBS += $$JpegPath\lib\jpeg-static.lib

    JpegXRPath = $$PWD\..\3rdparty\jxrlib
    LIBS += $$JpegXRPath\lib\JXRGlueLib.lib $$JpegXRPath\lib\JXRDecodeLib.lib $$JpegXRPath\lib\JXREncodeLib.lib $$JpegXRPath\lib\JXRCommonLib.lib

    FreeImagePath = "$$PWD\..\3rdparty\freeimage"
    INCLUDEPATH += $$FreeImagePath
    LIBS += $$FreeImagePath\FreeImagePlus.lib $$FreeImagePath\FreeImage.lib
    QMAKE_POST_LINK += robocopy /is $$FreeImagePath $$OUT_PWD/release *.dll &

    HDF5Path = "$$PWD\..\3rdparty\hdf5"
    INCLUDEPATH += $$HDF5Path\include
    LIBS += $$HDF5Path\lib\libhdf5_cpp.lib $$HDF5Path\lib\libhdf5.lib

    AssimpPath = "$$PWD\..\3rdparty\assimp"
    INCLUDEPATH += $$AssimpPath\include
    LIBS += $$AssimpPath\lib\assimp-vc120-mt.lib
    QMAKE_POST_LINK += robocopy /is $$AssimpPath\bin $$OUT_PWD/release *.dll &

    ZlibPath = "$$PWD\..\3rdparty\zlib"
    INCLUDEPATH += $$ZlibPath\include
    LIBS += $$ZlibPath\lib\zlibstatic.lib

    LIBS += "C:\Program Files (x86)\Intel\Composer XE\compiler\lib\intel64\libiomp5md.lib" \
        "C:\Program Files (x86)\Intel\Composer XE\compiler\lib\intel64\libirc.lib"
#        "odbc32.lib" "odbccp32.lib" "wbemuuid.lib" "d3d9.lib" "kernel32.lib" "user32.lib" "gdi32.lib" \
#        "winspool.lib" "comdlg32.lib" "advapi32.lib" "shell32.lib" "ole32.lib" "oleaut32.lib" "uuid.lib"

    RC_FILE = icons/app.rc
}

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../img/release/ -limg
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../img/debug/ -limg
else:unix: LIBS += -L$$OUT_PWD/../img/ -limg

INCLUDEPATH += $$PWD/../img
DEPENDPATH += $$PWD/../img

win32-g++:CONFIG(release, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../img/release/libimg.a
else:win32-g++:CONFIG(debug, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../img/debug/libimg.a
else:win32:!win32-g++:CONFIG(release, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../img/release/img.lib
else:win32:!win32-g++:CONFIG(debug, debug|release): PRE_TARGETDEPS += $$OUT_PWD/../img/debug/img.lib
else:unix: PRE_TARGETDEPS += $$OUT_PWD/../img/libimg.a
