cmake_minimum_required(VERSION 3.5.2)
project(Atlas)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.8)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake_modules)

if (NOT CMAKE_DEBUG_POSTFIX)
  set(CMAKE_DEBUG_POSTFIX d)
endif ()
message(${CMAKE_DEBUG_POSTFIX})

# Change the default build type from Debug to Release, while still
# supporting overriding the build type.
#
# The CACHE STRING logic here and elsewhere is needed to force CMake
# to pay attention to the value of these variables.
if (NOT CMAKE_BUILD_TYPE)
  message("-- No build type specified; defaulting to CMAKE_BUILD_TYPE=Release.")
  set(CMAKE_BUILD_TYPE Release CACHE STRING
      "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel."
      FORCE)
else (NOT CMAKE_BUILD_TYPE)
  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    message("\n=================================================================================")
    message("\n-- Build type: Debug. Performance will be terrible!")
    message("-- Add -DCMAKE_BUILD_TYPE=Release to the CMake command line to get an optimized build.")
    message("\n=================================================================================")
  endif (CMAKE_BUILD_TYPE STREQUAL "Debug")
endif (NOT CMAKE_BUILD_TYPE)

# main stuff

if (WIN32)
  set(INTEL_PATH "C:\\Program Files (x86)\\Intel\\Composer XE\\compiler")
  set(MKL_PATH "C:\\Program Files (x86)\\Intel\\Composer XE\\mkl")
else (WIN32)
  set(INTEL_PATH /opt/intel)
  set(MKL_PATH ${INTEL_PATH}/mkl)
endif (WIN32)
set(MKL_INCLUDE_DIRS ${MKL_INCLUDE_DIRS} ${MKL_PATH}/include ${MKL_PATH}/include/fftw)
if (WIN32)
  set(MKL_LIBRARIES ${MKL_LIBRARIES}
      ${MKL_PATH}/lib/intel64/mkl_intel_lp64.lib
      ${MKL_PATH}/lib/intel64/mkl_intel_thread.lib
      ${MKL_PATH}/lib/intel64/mkl_core.lib
      ${INTEL_PATH}/lib/intel64/libiomp5md.lib)
else (WIN32)
  set(MKL_LIBRARIES ${MKL_LIBRARIES}
      ${MKL_PATH}/lib/libmkl_intel_lp64.a
      ${MKL_PATH}/lib/libmkl_intel_thread.a
      ${MKL_PATH}/lib/libmkl_core.a
      ${INTEL_PATH}/lib/libiomp5.dylib)
endif (WIN32)

if (WIN32)
  set(IPP_PATH "C:\\Program Files (x86)\\Intel\\Composer XE\\ipp")
else (WIN32)
  set(IPP_PATH ${INTEL_PATH}/ipp)
endif (WIN32)
set(IPP_INCLUDE_DIRS ${IPP_INCLUDE_DIRS} ${IPP_PATH}/include)
if (WIN32)
  set(IPP_LIBRARIES ${IPP_LIBRARIES}
      ${IPP_PATH}/lib/intel64/ippimt.lib
      ${IPP_PATH}/lib/intel64/ippcoremt.lib
      ${IPP_PATH}/lib/intel64/ippvmmt.lib
      ${IPP_PATH}/lib/intel64/ippsmt.lib
      ${IPP_PATH}/lib/intel64/ippcvmt.lib
      ${IPP_PATH}/lib/intel64/ippccmt.lib
      ${INTEL_PATH}/lib/intel64/libirc.lib)
else (WIN32)
  set(IPP_LIBRARIES ${IPP_LIBRARIES}
      ${IPP_PATH}/lib/libippi.a
      ${IPP_PATH}/lib/libippcore.a
      ${IPP_PATH}/lib/libippvm.a
      ${IPP_PATH}/lib/libipps.a
      ${IPP_PATH}/lib/libippcv.a
      ${IPP_PATH}/lib/libippcc.a
      ${INTEL_PATH}/lib/libirc.a)
endif (WIN32)

find_package(VTK REQUIRED COMPONENTS vtkFiltersGeometry vtkFiltersSources
             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/vtk NO_DEFAULT_PATH)
message(${VTK_DIR})
message(${VTK_USE_FILE})
include(${VTK_USE_FILE})
message("${VTK_INCLUDE_DIRS}")
message("${VTK_LIBRARIES}")

set(WILDMAGIC_INCLUDE_DIRS ${WILDMAGIC_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/wildmagic/include)
if (WIN32)
  set(WILDMAGIC_LIBRARIES ${WILDMAGIC_LIBRARIES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/wildmagic/lib/GTEngine.lib)
else (WIN32)
  set(WILDMAGIC_LIBRARIES ${WILDMAGIC_LIBRARIES}
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/wildmagic/lib/libGTEngine.a)
endif (WIN32)

find_package(HDF5 REQUIRED COMPONENTS C CXX static
             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/hdf5/share NO_DEFAULT_PATH)
message(${HDF5_INCLUDE_DIR})
message(${HDF5_C_STATIC_LIBRARY})
message(${HDF5_CXX_STATIC_LIBRARY})

find_package(ASSIMP REQUIRED
             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/assimp NO_DEFAULT_PATH)
message(${ASSIMP_INCLUDE_DIRS})
message(${ASSIMP_LIBRARY_DIRS})
message(${ASSIMP_LIBRARIES})
link_directories(${ASSIMP_LIBRARY_DIRS})  # need to do this before creating target

find_package(OpenCV REQUIRED COMPONENTS imgproc core
             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/opencv NO_DEFAULT_PATH)
message("${OpenCV_INCLUDE_DIRS}")
message("${OpenCV_LIBS}")

find_package(GLBINDING REQUIRED
             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/glbinding NO_DEFAULT_PATH)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5Core REQUIRED PATHS /Users/feng/Qt/5.7/clang_64 NO_DEFAULT_PATH)
find_package(Qt5Widgets REQUIRED PATHS /Users/feng/Qt/5.7/clang_64 NO_DEFAULT_PATH)
find_package(Qt5Network REQUIRED PATHS /Users/feng/Qt/5.7/clang_64 NO_DEFAULT_PATH)
find_package(Qt5Svg REQUIRED PATHS /Users/feng/Qt/5.7/clang_64 NO_DEFAULT_PATH)

# get_cmake_property(_variableNames VARIABLES)
# foreach (_variableName ${_variableNames})
#   message(STATUS "${_variableName}=${${_variableName}}")
# endforeach()

include(${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/ceres-solver/ceres-solver.cmake)

#libqxt srcs
add_definitions(-DBUILD_QXT -DBUILD_QXT_OBJECT)
set(LIBQXTDIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libqxt)
set(LIBQXT_HEADERS
    ${LIBQXTDIR}/core/qxtglobal.h
    ${LIBQXTDIR}/core/qxtcsvmodel.h
    ${LIBQXTDIR}/widgets/qxtspanslider.h
    ${LIBQXTDIR}/widgets/qxtspanslider_p.h
    ${LIBQXTDIR}/core/qxtnamespace.h
    )
set(LIBQXT_SOURCES
    ${LIBQXTDIR}/core/qxtglobal.cpp
    ${LIBQXTDIR}/core/qxtcsvmodel.cpp
    ${LIBQXTDIR}/widgets/qxtspanslider.cpp
    )
source_group(libqxt FILES ${LIBQXT_HEADERS} ${LIBQXT_SOURCES})

#qtcsv srcs
set(QTCSVDIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/qtcsv)
set(QTCSV_HEADERS
    ${QTCSVDIR}/include/qtcsv_global.h
    ${QTCSVDIR}/include/writer.h
    ${QTCSVDIR}/include/variantdata.h
    ${QTCSVDIR}/include/stringdata.h
    ${QTCSVDIR}/include/reader.h
    ${QTCSVDIR}/include/abstractdata.h
    ${QTCSVDIR}/sources/filechecker.h
    ${QTCSVDIR}/sources/contentiterator.h
    ${QTCSVDIR}/sources/symbols.h
    )
set(QTCSV_SOURCES
    ${QTCSVDIR}/sources/writer.cpp
    ${QTCSVDIR}/sources/variantdata.cpp
    ${QTCSVDIR}/sources/stringdata.cpp
    ${QTCSVDIR}/sources/reader.cpp
    ${QTCSVDIR}/sources/contentiterator.cpp
    )
source_group(qtcsv FILES ${QTCSV_HEADERS} ${QTCSV_SOURCES})

# video memory sources
set(SYSDIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/sys)
if (WIN32)
  set(SYS_SOURCES
      ${SYSDIR}/VideoMemoryWin.cpp
      ${SYSDIR}/VidMemViaD3D9.cpp
      ${SYSDIR}/VidMemViaDDraw.cpp
      ${SYSDIR}/VidMemViaDxDiag.cpp
      )
else (WIN32)
  set(SYS_SOURCES
      ${SYSDIR}/VideoMemoryMac.cpp
      )
endif (WIN32)
source_group(sys FILES ${SYS_SOURCES})

# main srcs

if (WIN32)
  set(ASSETS
      ${CMAKE_CURRENT_SOURCE_DIR}/icons/app.rc)
endif ()
if (APPLE)
  set(ASSETS
      ${CMAKE_CURRENT_SOURCE_DIR}/icons/app.icns
      ${CMAKE_CURRENT_SOURCE_DIR}/../../ffmpeg)
  set(MACOSX_BUNDLE_ICON_FILE app.icns)
endif ()

set(FORMS
    LogDialog.ui
    )

set(RESOURCES
    LogDialog.qrc
    atlas.qrc
    )

option(BUILD_TESTING "Enable tests" ON)
if (BUILD_TESTING)
  add_definitions(-D_WITH_TESTS_)
  set(TESTS
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/googletest/src/gtest-all.cc
      test/zimggraphtest.h
      test/zimagetoimagemetrictest.h
      test/zimageutilstest.h
      test/zimageaffinetransformtest.h
      test/zimgncctest.h
      test/zfilereadtest.h
      test/zimgiteratortest.h
      test/zsaturateoperationtest.h
      test/zclustertest.h
      test/zimgtest.h
      test/zimgconnectedcomponentstest.h
      test/zimgsigneddistancemaptest.h
      test/zimgautothresholdtest.h
      test/zimgregionalextrematest.h
      zunittest.h
      zunittest.cpp
      )
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/googletest/include)
endif (BUILD_TESTING)

set(SOURCES
    zimageinterpolation.h
    zimgdisplay.h
    zimgmerge.h
    zimgtile.h
    zvoxelregion.h
    zimgncc.h
    zcompleximg.h
    zfft.h
    zimagetoimagemetric.h
    zregistrationoptimizer.h
    zregistrationcostfunction.h
    zsectionsregistrationdialog.h
    zsectionsregistration.h
    zstitchimagedialog.h
    zpunctadetectiondialog.h
    zpunctadetection.h
    zassignpuncta.h
    zgenerateanalysistextfile.h
    zanalysisworklistdialog.h
    zanalysisworklistmodel.h
    zstyleditemdelegate.h
    zitemeditorfactory.h
    zimgalgorithm.h
    zimgconnectedcomponents.h
    zimgregionalextrema.h
    zimgautothreshold.h
    zimgitkinterface.h
    zimgprocess.h
    zimgsigneddistancemap.h
    zimggraph.h
    zimgnccmatch.h
    zmainwindow.h
    zimgdoc.h
    zimgview.h
    zspinboxwithscrollbar.h
    zgraphicsview.h
    zview.h
    zdoc.h
    zobjdoc.h
    zobjview.h
    zimgfilter.h
    zobjmodel.h
    zobjwidget.h
    zshareitemselectionmodel.h
    zitemselectionmodel.h
    zviewsettingwidget.h
    z3dmainwindow.h
    z3dview.h
    z3dobjview.h
    z3dimgview.h
    zviewsettinginterface.h
    zpunctadoc.h
    z3dpunctaview.h
    z3dpunctafilter.h
    z3dmeshfilter.h
    z3dswcfilter.h
    zmeshdoc.h
    zswcdoc.h
    z3dswcview.h
    z3dmeshview.h
    zobjfilter.h
    zanimation.h
    zparameteranimation.h
    zparameterkey.h
    zcameraparameterkey.h
    zcameraparameteranimation.h
    z3danimation.h
    z3danimationdoc.h
    zsaveobjsdialog.h
    zanimationwidget.h
    zobjeditwidget.h
    zparameterfactory.h
    zfontparameter.h
    zfontwidget.h
    z2danimation.h
    z2danimationdoc.h
    ztimelinewidget.h
    ztimelineobjview.h
    ztimelineeventview.h
    ztimelineobjscene.h
    ztimelineeventscene.h
    ztimelineaxisview.h
    ztimelinekeyeditdialog.h
    zdockwidget.h
    z3danimationview.h
    z3danimationfilter.h
    z3dcompositor.h
    z3dgeometryfilter.h
    z3daxisfilter.h
    z3dboundedfilter.h
    z3dvolumeslicerenderer.h
    z3dmeshrenderer.h
    z3dtexturecopyrenderer.h
    z3dtexturecoordinaterenderer.h
    z3dtextureblendrenderer.h
    z3dsphererenderer.h
    z3dlinewithfixedwidthcolorrenderer.h
    z3dlinerenderer.h
    z3dimage2drenderer.h
    z3dfontrenderer.h
    z3dellipsoidrenderer.h
    z3dconerenderer.h
    z3dbackgroundrenderer.h
    z3darrowrenderer.h
    z3drendererbase.h
    z3dprimitiverenderer.h
    z3dcanvaspainter.h
    z3dnetworkevaluator.h
    z3dcanvas.h
    z3dscene.h
    z3dshadergroup.h
    z3dport.h
    z3drenderport.h
    z3dglobalparameters.h
    z3dshaderprogram.h
    z3dvolume.h
    z3dtransferfunction.h
    z3dtransferfunctioneditor.h
    z3dtransferfunctionwidgetwitheditorwindow.h
    zclickablelabel.h
    zcolormap.h
    zcolormapeditor.h
    zcolormapwidgetwitheditorwindow.h
    zvertexarrayobject.h
    zvertexbufferobject.h
    z3dpickingmanager.h
    z3dfilterview.h
    zroi.h
    zroidoc.h
    zfilterview.h
    zroiview.h
    zroifilter.h
    zactiongroup.h
    zboostgeometryadaptor.h
    zquatparameter.h
    z3dshadermanager.h
    zanimationexportwidget.h
    zvideoencoder.h
    zmesh.h
    zmeshio.h
    zmeshutils.h
    zpuncta.h
    zpunctaio.h
    zpunctum.h
    zgmm.h
    zkmeans.h
    zvbgmm.h
    zswcfilter.h
    zswcview.h
    zpunctafilter.h
    zpunctaview.h
    zswc.h
    ztree.hpp
    znumericparameter.h
    zoptionparameter.h
    zparameter.h
    zglmutils.h
    zcombobox.h
    zselectfilewidget.h
    zloadimagesequencedialog.h
    zspinbox.h
    zspinboxwithslider.h
    zwidgetsgroup.h
    z3dcamera.h
    z3dcameraparameter.h
    z3drendertarget.h
    z3dinteractionhandler.h
    zeventlistenerparameter.h
    z3dcanvaseventlistener.h
    z3dtexture.h
    z3dtransformparameter.h
    z3dgpuinfo.h
    zstringparameter.h
    z3dsdfont.h
    zspanslider.h
    zcustomcommand.h
    zaffine2d.h
    zaffine3d.h
    ztakescreenshotwidget.h
    zimgregistration.h
    zimagetransform.h
    zimagematrix2dtransform.h
    zimagematrix3dtransform.h
    zregistrationnumericdiffcostfunction.h
    zimagecompositetransform.h
    zimagetransformresolve.h
    z3dshader.h
    z3dcontext.h
    z3dgl.h
    zimgpack.h
    zimgpackdisplay.h
    zimgvoxelcolormap.h
    zimgcache.h
    zsysteminfo.h
    zfileutils.h
    zregionannotation.h
    zregionannotationdoc.h
    z3dregionannotationview.h
    zregionannotationview.h
    zregionannotationfilter.h
    z3dregionannotationfilter.h
    zimgfillhole.h
    zregionannotationwidget.h
    zregionannotationtreemodel.h
    zregionannotationtreeview.h
    zbuttoncolumndelegate.h
    zchooseobjdialog.h
    zgraphicsscene.h
    z3dfilter.h
    z3dtextureglowrenderer.h
    z3dcameracontrolwidget.h
    z3dtextureandeyecoordinaterenderer.h
    z3dimgraycasterrenderer.h
    z3dimg.h
    z3dimgfilter.h
    z3dblockcache.h
    z3dimgslicerenderer.h
    zregionontology.h
    zobjdetailedinfowidget.h
    zjson.h
    zlogdialog.h
    zlogmodelsink.h
    main.cpp
    zimageinterpolation.cpp
    zimgdisplay.cpp
    zimgmerge.cpp
    zimgtile.cpp
    zvoxelregion.cpp
    zimgncc.cpp
    zcompleximg.cpp
    zfft.cpp
    zimagetoimagemetric.cpp
    zregistrationoptimizer.cpp
    zregistrationcostfunction.cpp
    zsectionsregistrationdialog.cpp
    zsectionsregistration.cpp
    zstitchimagedialog.cpp
    zpunctadetectiondialog.cpp
    zpunctadetection.cpp
    zassignpuncta.cpp
    zgenerateanalysistextfile.cpp
    zanalysisworklistdialog.cpp
    zanalysisworklistmodel.cpp
    zstyleditemdelegate.cpp
    zitemeditorfactory.cpp
    zimgalgorithm.cpp
    zimgprocess.cpp
    zimggraph.cpp
    zimgnccmatch.cpp
    zimgsigneddistancemap.cpp
    zimgconnectedcomponents.cpp
    zimgautothreshold.cpp
    zimgregionalextrema.cpp
    zmainwindow.cpp
    zimgdoc.cpp
    zimgview.cpp
    zspinboxwithscrollbar.cpp
    zgraphicsview.cpp
    zview.cpp
    zdoc.cpp
    zobjdoc.cpp
    zobjview.cpp
    zimgfilter.cpp
    zobjmodel.cpp
    zobjwidget.cpp
    zshareitemselectionmodel.cpp
    zitemselectionmodel.cpp
    zviewsettingwidget.cpp
    z3dmainwindow.cpp
    z3dview.cpp
    z3dobjview.cpp
    z3dimgview.cpp
    zpunctadoc.cpp
    z3dpunctaview.cpp
    z3dpunctafilter.cpp
    z3dmeshfilter.cpp
    z3dswcfilter.cpp
    zmeshdoc.cpp
    zswcdoc.cpp
    z3dswcview.cpp
    z3dmeshview.cpp
    zobjfilter.cpp
    zanimation.cpp
    zparameteranimation.cpp
    zparameterkey.cpp
    zcameraparameterkey.cpp
    zcameraparameteranimation.cpp
    z3danimation.cpp
    z3danimationdoc.cpp
    zsaveobjsdialog.cpp
    zanimationwidget.cpp
    zobjeditwidget.cpp
    zparameterfactory.cpp
    zfontparameter.cpp
    zfontwidget.cpp
    z2danimation.cpp
    z2danimationdoc.cpp
    ztimelinewidget.cpp
    ztimelineobjview.cpp
    ztimelineeventview.cpp
    ztimelineobjscene.cpp
    ztimelineeventscene.cpp
    ztimelineaxisview.cpp
    ztimelinekeyeditdialog.cpp
    zdockwidget.cpp
    z3danimationview.cpp
    z3danimationfilter.cpp
    z3dcompositor.cpp
    z3dgeometryfilter.cpp
    z3daxisfilter.cpp
    z3dboundedfilter.cpp
    z3dvolumeslicerenderer.cpp
    z3dmeshrenderer.cpp
    z3dtexturecopyrenderer.cpp
    z3dtexturecoordinaterenderer.cpp
    z3dtextureblendrenderer.cpp
    z3dsphererenderer.cpp
    z3dprimitiverenderer.cpp
    z3dlinewithfixedwidthcolorrenderer.cpp
    z3dlinerenderer.cpp
    z3dimage2drenderer.cpp
    z3dfontrenderer.cpp
    z3dellipsoidrenderer.cpp
    z3dconerenderer.cpp
    z3dbackgroundrenderer.cpp
    z3darrowrenderer.cpp
    z3drendererbase.cpp
    z3dcanvaspainter.cpp
    z3dnetworkevaluator.cpp
    z3dcanvas.cpp
    z3dscene.cpp
    z3dshadergroup.cpp
    z3dport.cpp
    z3drenderport.cpp
    z3dglobalparameters.cpp
    z3dshaderprogram.cpp
    z3dvolume.cpp
    z3dtransferfunction.cpp
    z3dtransferfunctioneditor.cpp
    z3dtransferfunctionwidgetwitheditorwindow.cpp
    zclickablelabel.cpp
    zcolormap.cpp
    zcolormapeditor.cpp
    zcolormapwidgetwitheditorwindow.cpp
    zvertexarrayobject.cpp
    zvertexbufferobject.cpp
    z3dpickingmanager.cpp
    zroi.cpp
    zroidoc.cpp
    zroiview.cpp
    zroifilter.cpp
    zactiongroup.cpp
    zquatparameter.cpp
    z3dshadermanager.cpp
    zanimationexportwidget.cpp
    zvideoencoder.cpp
    zmesh.cpp
    zmeshio.cpp
    zmeshutils.cpp
    zpuncta.cpp
    zpunctaio.cpp
    zpunctum.cpp
    zswcfilter.cpp
    zswcview.cpp
    zpunctafilter.cpp
    zpunctaview.cpp
    zswc.cpp
    znumericparameter.cpp
    zoptionparameter.cpp
    zparameter.cpp
    zcombobox.cpp
    zselectfilewidget.cpp
    zloadimagesequencedialog.cpp
    zspinbox.cpp
    zspinboxwithslider.cpp
    zwidgetsgroup.cpp
    z3dcamera.cpp
    z3dcameraparameter.cpp
    z3drendertarget.cpp
    z3dinteractionhandler.cpp
    zeventlistenerparameter.cpp
    z3dtexture.cpp
    z3dtransformparameter.cpp
    z3dgpuinfo.cpp
    zstringparameter.cpp
    z3dsdfont.cpp
    zspanslider.cpp
    zcustomcommand.cpp
    zaffine2d.cpp
    zaffine3d.cpp
    ztakescreenshotwidget.cpp
    zimgregistration.cpp
    zimagetransform.cpp
    zimagematrix2dtransform.cpp
    zimagematrix3dtransform.cpp
    zregistrationnumericdiffcostfunction.cpp
    zimagecompositetransform.cpp
    zimagetransformresolve.cpp
    z3dshader.cpp
    z3dcontext.cpp
    z3dgl.cpp
    zimgpack.cpp
    zimgpackdisplay.cpp
    zimgcache.cpp
    zsysteminfo.cpp
    zfileutils.cpp
    zregionannotation.cpp
    zregionannotationdoc.cpp
    z3dregionannotationview.cpp
    zregionannotationview.cpp
    zregionannotationfilter.cpp
    z3dregionannotationfilter.cpp
    zimgfillhole.cpp
    zregionannotationwidget.cpp
    zregionannotationtreemodel.cpp
    zregionannotationtreeview.cpp
    zbuttoncolumndelegate.cpp
    zchooseobjdialog.cpp
    zgraphicsscene.cpp
    z3dfilter.cpp
    z3dtextureglowrenderer.cpp
    z3dcameracontrolwidget.cpp
    z3dtextureandeyecoordinaterenderer.cpp
    z3dimgraycasterrenderer.cpp
    z3dimg.cpp
    z3dimgfilter.cpp
    z3dimgslicerenderer.cpp
    zregionontology.cpp
    zobjdetailedinfowidget.cpp
    zjson.cpp
    zlogdialog.cpp
    zlogmodelsink.cpp
    zapplication.cpp zapplication.h)

# our target
set(APP_NAME Atlas)
add_executable(${APP_NAME} WIN32 MACOSX_BUNDLE
               ${SYS_SOURCES}
               ${QTCSV_HEADERS} ${QTCSV_SOURCES}
               ${LIBQXT_HEADERS} ${LIBQXT_SOURCES}
               ${CERESSOLVER_HEADERS} ${CERESSOLVER_INTERNAL}
               ${SOURCES} ${FORMS} ${RESOURCES} ${ASSETS}
               ${TESTS})

target_include_directories(${APP_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${APP_NAME} PUBLIC ${TBB_INCLUDE_DIRS})
target_include_directories(${APP_NAME} PUBLIC ${MKL_INCLUDE_DIRS})
target_include_directories(${APP_NAME} PUBLIC ${IPP_INCLUDE_DIRS})
target_include_directories(${APP_NAME} PUBLIC ${WILDMAGIC_INCLUDE_DIRS})
target_include_directories(${APP_NAME} PUBLIC ${HDF5_INCLUDE_DIR})
target_include_directories(${APP_NAME} PUBLIC ${ASSIMP_INCLUDE_DIRS})
target_include_directories(${APP_NAME} PUBLIC ${CERESSOLVER_INCLUDE_DIRS})
target_include_directories(${APP_NAME} PUBLIC ${QTCSVDIR})
target_include_directories(${APP_NAME} PUBLIC ${LIBQXTDIR}/core ${LIBQXTDIR}/widgets)
target_include_directories(${APP_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/eigen)
target_include_directories(${APP_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/boost)
target_include_directories(${APP_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/glm)
target_include_directories(${APP_NAME} SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/folly)
target_compile_definitions(${APP_NAME} PUBLIC EIGEN_NO_DEBUG EIGEN_DONT_PARALLELIZE EIGEN_MPL2_ONLY)
target_compile_definitions(${APP_NAME} PUBLIC EIGEN_USE_BLAS EIGEN_USE_LAPACKE_STRICT EIGEN_USE_MKL_VML)
target_compile_definitions(${APP_NAME} PUBLIC QT_USE_QSTRINGBUILDER QT_NO_CAST_FROM_BYTEARRAY QT_NO_CAST_TO_ASCII)
target_compile_definitions(${APP_NAME} PUBLIC _USE_CORE_PROFILE_ _USE_MKL_ _USE_IPP_)
if (WIN32)
  target_compile_definitions(${APP_NAME} PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)

  target_compile_options(${APP_NAME} PUBLIC /GR) # Enable Run-Time Type Information
  target_compile_options(${APP_NAME} PUBLIC /EHsc) # exception handling model, https://msdn.microsoft.com/en-us/library/1deeycx5.aspx
  target_compile_options(${APP_NAME} PUBLIC /Wall)
  target_compile_options(${APP_NAME} PUBLIC /bigobj /GL) # Enables whole program optimization.
  target_compile_options(${APP_NAME} PUBLIC /wd4267) # 'var' : conversion from 'size_t' to 'type', possible loss of data
  target_compile_options(${APP_NAME} PUBLIC /wd4244) # 'argument' : conversion from 'type1' to 'type2', possible loss of data
  target_compile_options(${APP_NAME} PUBLIC /wd4305) # 'identifier' : truncation from 'type1' to 'type2'
  target_compile_options(${APP_NAME} PUBLIC /wd4819) # The file contains a character that cannot be represented in the current code page (number). Save the file in Unicode format to prevent data loss.
  target_compile_options(${APP_NAME} PUBLIC /utf-8)  # https://blogs.msdn.microsoft.com/vcblog/2016/02/22/new-options-for-managing-character-sets-in-the-microsoft-cc-compiler/

  target_link_libraries(${APP_NAME} PUBLIC /LTCG) # Link-time Code Generation

  # for some files
  set_source_files_properties(${CERESSOLVER_INTERNAL} PROPERTIES COMPILE_FLAGS
                              "/wd4100 /I${CERESSOLVER_INTERNAL_INCLUDE_DIRS}") # 'identifier' : unreferenced formal parameter)
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/googletest/src/gtest-all.cc
                              PROPERTIES COMPILE_FLAGS /I${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/googletest)
else (WIN32)
  target_compile_options(${APP_NAME} PUBLIC -frtti)
  target_compile_options(${APP_NAME} PUBLIC -fexceptions)
  target_compile_options(${APP_NAME} PUBLIC -Wall -Wextra)
  target_compile_options(${APP_NAME} PUBLIC -mllvm -inline-threshold=600 -stdlib=libc++)

  target_link_libraries(${APP_NAME} PUBLIC -stdlib=libc++ "-rpath @executable_path/../Frameworks")

  # for some files
  set_source_files_properties(${CERESSOLVER_INTERNAL} PROPERTIES COMPILE_FLAGS
                              "-Wno-unused-parameter -Wno-sign-compare -I${CERESSOLVER_INTERNAL_INCLUDE_DIRS}")
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/googletest/src/gtest-all.cc
                              PROPERTIES COMPILE_FLAGS -I${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/googletest)

  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/icons/app.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../../ffmpeg PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif (WIN32)

target_link_libraries(${APP_NAME} PUBLIC ${VTK_LIBRARIES} glbinding::glbinding hdf5_cpp-static hdf5-static
                      img ${OpenCV_LIBS} Qt5::Core Qt5::Widgets Qt5::Network Qt5::Svg ${WILDMAGIC_LIBRARIES}
                      ${ASSIMP_LIBRARIES} ${MKL_LIBRARIES} ${IPP_LIBRARIES} ${TBB_LIBRARIES})

if (APPLE)
  set(copyright_line "Copyright 2011-2016 Linqing Feng @ JinnyLab")

  set_target_properties(${APP_NAME} PROPERTIES
                        OUTPUT_NAME "${APP_NAME}"
                        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Info.plist.in"
                        MACOSX_BUNDLE_SHORT_VERSION_STRING "${CMAKE_BUNDLE_VERSION}"
                        # TBD: MACOSX_BUNDLE_BUNDLE_VERSION "${CMAKE_BUNDLE_VERSION}"
                        MACOSX_BUNDLE_COPYRIGHT "${copyright_line}"
                        MACOSX_BUNDLE_GUI_IDENTIFIER "org.atlas.Atlas"
                        )
endif (APPLE)

