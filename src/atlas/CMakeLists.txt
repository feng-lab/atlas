cmake_minimum_required(VERSION 3.17.5)
project(Atlas)
set(ATLAS_VERSION ${GIT_VERSION})

LIST(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

cmake_policy(SET CMP0074 NEW)

# main stuff

## OpenSSL installation directory can be configured by setting OPENSSL_ROOT_DIR
## We expect to locate OpenSSL using the built-in cmake module as the openssl
## project itself does not provide installation support in its CMakeLists.txt
## See https://cmake.org/cmake/help/v3.6/module/FindOpenSSL.html
#set(OPENSSL_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build)
#find_package(OpenSSL REQUIRED)
#if (TARGET OpenSSL::SSL)
#  print_target_properties(OpenSSL::SSL)
#  print_target_properties(OpenSSL::Crypto)
#  set(_Atlas_SSL_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
#else ()
#  # set(_Atlas_SSL_LIBRARIES ${OPENSSL_LIBRARIES})
#  message(FATAL_ERROR "OpenSSL targets not found")
#endif ()
#set(_Atlas_SSL_INCLUDE_DIR ${OPENSSL_INCLUDE_DIR})
#message(STATUS "_Atlas_SSL_INCLUDE_DIR: ${_Atlas_SSL_INCLUDE_DIR}")
#message(STATUS "_Atlas_SSL_LIBRARIES: ${_Atlas_SSL_LIBRARIES}")

# Find Protobuf installation
# Looks for protobuf-config.cmake file installed by Protobuf's cmake installation.
find_package(Protobuf CONFIG REQUIRED
             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build NO_DEFAULT_PATH)
message(STATUS "Using Protobuf ${Protobuf_VERSION}")
print_target_properties(protobuf::libprotobuf)
print_target_properties(protobuf::protoc)
set(protobuf_PROTOC_EXE protobuf::protoc) # for protobuf_generate

# Find gRPC installation
# Looks for gRPCConfig.cmake file installed by gRPC's cmake installation.
find_package(gRPC CONFIG REQUIRED
             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build NO_DEFAULT_PATH)
message(STATUS "Using gRPC ${gRPC_VERSION}")
print_target_properties(gRPC::grpc++)
print_target_properties(gRPC::grpc_cpp_plugin)

include(gRPCGenerateCPP)

set(PROTOS
    ${CMAKE_CURRENT_SOURCE_DIR}/../protos/helloworld.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/../protos/imgprocess.proto
    )
set(PROTO_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../protos)
protobuf_generate(LANGUAGE cpp APPEND_PATH PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR} OUT_VAR PROTO_SRCS PROTOS ${PROTOS})
grpc_generate_cpp(GRPC_SRCS GRPC_HDRS ${CMAKE_CURRENT_BINARY_DIR} ${PROTOS})
set_property(SOURCE ${PROTO_SRCS} ${GRPC_SRCS} PROPERTY SKIP_AUTOGEN ON)

find_package(benchmark REQUIRED
             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(benchmark::benchmark)

#find_package(HDF5 REQUIRED COMPONENTS C CXX static
#             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build NO_DEFAULT_PATH)
#message(STATUS "HDF5_INCLUDE_DIR: ${HDF5_INCLUDE_DIR}")

set(OpenCV_STATIC ON)
find_package(OpenCV REQUIRED COMPONENTS imgproc core
             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build NO_DEFAULT_PATH)
message(STATUS "OpenCV_INCLUDE_DIRS: ${OpenCV_INCLUDE_DIRS}")
message(STATUS "OpenCV_LIBS: ${OpenCV_LIBS}")
print_target_properties(opencv_imgproc)
print_target_properties(opencv_core)

find_package(GLBINDING REQUIRED
             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build NO_DEFAULT_PATH)
print_target_properties(glbinding::glbinding-aux)
print_target_properties(glbinding::glbinding)
print_target_properties(glbinding::KHRplatform)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 ${QT_VERSION} REQUIRED COMPONENTS Core Widgets Network Svg SvgWidgets Concurrent OpenGLWidgets Test
             PATHS ${QT_HOST_PATH} NO_DEFAULT_PATH)
print_target_properties(Qt6::Core)
print_target_properties(Qt6::Widgets)
print_target_properties(Qt6::Network)
print_target_properties(Qt6::Svg)
print_target_properties(Qt6::SvgWidgets)
print_target_properties(Qt6::Concurrent)
print_target_properties(Qt6::OpenGLWidgets)
print_target_properties(Qt6::Test)

#find_package(RocksDB REQUIRED
#             PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build NO_DEFAULT_PATH)
#print_target_properties(RocksDB::rocksdb)

print_target_properties(img NO_LOCATION)

print_target_properties(neurolabi NO_LOCATION)

print_target_properties(neutu NO_LOCATION)

if (UNIX AND NOT APPLE)
  find_package(OpenGL REQUIRED COMPONENTS OpenGL EGL)
  print_target_properties(OpenGL::OpenGL)
  print_target_properties(OpenGL::EGL)
endif ()

# get_cmake_property(_variableNames VARIABLES)
# foreach (_variableName ${_variableNames})
#   message(STATUS "${_variableName}=${${_variableName}}")
# endforeach()

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
add_definitions(-DQTCSV_STATIC_LIB)
set(QTCSVDIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/qtcsv)
set(QTCSV_HEADERS
    ${QTCSVDIR}/include/qtcsv/qtcsv_global.h
    ${QTCSVDIR}/include/qtcsv/writer.h
    ${QTCSVDIR}/include/qtcsv/variantdata.h
    ${QTCSVDIR}/include/qtcsv/stringdata.h
    ${QTCSVDIR}/include/qtcsv/reader.h
    ${QTCSVDIR}/include/qtcsv/abstractdata.h
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
elseif (APPLE)
  set(SYS_SOURCES
      ${SYSDIR}/VideoMemoryMac.cpp
      )
else (WIN32)
  set(SYS_SOURCES
      ${SYSDIR}/VideoMemoryLinux.cpp
      )
endif (WIN32)
source_group(sys FILES ${SYS_SOURCES})

# main srcs

if (WIN32)
  set(ASSETS
      ${CMAKE_CURRENT_SOURCE_DIR}/icons/app.rc)
elseif (APPLE)
  set(ASSETS
      ${CMAKE_CURRENT_SOURCE_DIR}/icons/app.icns
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/ffmpeg
      ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/bin/h5ls
      ${CMAKE_CURRENT_SOURCE_DIR}/settings_flagfile.txt)
  set(MACOSX_BUNDLE_ICON_FILE app.icns)
endif (WIN32)

set(FORMS
    LogDialog.ui
    )

set(RESOURCES
    atlas.qrc
    )

set(SOURCES
    zimgdisplay.h
    zsectionsregistrationdialog.h
    zstitchimagedialog.h
    zpunctadetectiondialog.h
    zgenerateanalysistextfile.h
    zanalysisworklistdialog.h
    zanalysisworklistmodel.h
    zstyleditemdelegate.h
    zitemeditorfactory.h
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
    z3drenderingengine.h
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
    z3dvertexarrayobject.h
    z3dvertexbufferobject.h
    z3dpickingmanager.h
    z3dfilterview.h
    zroi.h
    zroidoc.h
    zfilterview.h
    zroiview.h
    zroifilter.h
    zboostgeometryadaptor.h
    zquatparameter.h
    z3dshadermanager.h
    zanimationexportwidget.h
    zvideoencoder.h
    zswcfilter.h
    zswcview.h
    zpunctafilter.h
    zpunctaview.h
    znumericparameter.h
    zoptionparameter.h
    zparameter.h
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
    ztakescreenshotwidget.h
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
    zlogdialog.h
    zlogmodelsink.h
    main.cpp
    zimgdisplay.cpp
    zsectionsregistrationdialog.cpp
    zstitchimagedialog.cpp
    zpunctadetectiondialog.cpp
    zgenerateanalysistextfile.cpp
    zanalysisworklistdialog.cpp
    zanalysisworklistmodel.cpp
    zstyleditemdelegate.cpp
    zitemeditorfactory.cpp
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
    z3drenderingengine.cpp
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
    z3dvertexarrayobject.cpp
    z3dvertexbufferobject.cpp
    z3dpickingmanager.cpp
    zroi.cpp
    zroidoc.cpp
    zroiview.cpp
    zroifilter.cpp
    zquatparameter.cpp
    z3dshadermanager.cpp
    zanimationexportwidget.cpp
    zvideoencoder.cpp
    zswcfilter.cpp
    zswcview.cpp
    zpunctafilter.cpp
    zpunctaview.cpp
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
    ztakescreenshotwidget.cpp
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
    zlogdialog.cpp
    zlogmodelsink.cpp
    zapplication.cpp
    zapplication.h
    zsvgdoc.cpp
    zsvgdoc.h
    zsvgfilter.cpp
    zsvgfilter.h
    zsvgview.cpp
    zsvgview.h
    zgraphicsitemtype.h
    zparametereditdialog.cpp
    zparametereditdialog.h
    z2dtransformparameter.cpp
    z2dtransformparameter.h
    zspinboxdelegate.cpp
    zspinboxdelegate.h
    z3dopenglwidget.cpp
    z3dopenglwidget.h
    zchromaticshiftcorrectiondialog.cpp
    zchromaticshiftcorrectiondialog.h
    zlogwidget.cpp
    zlogwidget.h
    zimgprocessdialog.cpp
    zimgprocessdialog.h
    zservicemanager.cpp
    zservicemanager.h
    zrpcservice.cpp
    zrpcservice.h
    ztheme.cpp
    ztheme.h
    zserializationutils.h
    zroiwidget.cpp
    zroiwidget.h
    zgraphicsitemgroup.cpp
    zgraphicsitemgroup.h
    zchooseregiondialog.cpp
    zchooseregiondialog.h
    zregionannotationviewsettingtreemodel.h
    zregionannotationviewsettingtreeview.h
    zregionannotationviewsettingcolumndelegate.h
    zregionannotationviewsettingtreemodel.cpp
    zregionannotationviewsettingtreeview.cpp
    zregionannotationviewsettingcolumndelegate.cpp
    zregionviewsettingwitheditorwindow.h
    zregionviewsettingwitheditorwindow.cpp
    zpunctawidget.cpp
    zpunctawidget.h
    zpunctatablemodel.h
    zpunctatablemodel.cpp
    zpunctatableview.cpp
    zpunctatableview.h
    zpunctapack.cpp
    zpunctapack.h
    zswcpack.cpp
    zswcpack.h
    zswcwidget.cpp
    zswcwidget.h
    zswctreeview.cpp
    zswctreeview.h
    zswctreemodel.cpp
    zswctreemodel.h
    zobjpack.cpp
    zobjpack.h
    zroipack.cpp
    zroipack.h
    zregionannotationpack.cpp
    zregionannotationpack.h
    zswccolorparameters.cpp
    zswccolorparameters.h
    zthreadsafelrucache.h
    zthreadsafescalablecache.h
    zrunbenchmark.cpp zrunbenchmark.h
    zimgregioncache.cpp zimgregioncache.h
    zcheckbox.cpp zcheckbox.h
    zrunexport3danimation.cpp zrunexport3danimation.h
    zcancellation.h zrunneutucommand.cpp zrunneutucommand.h)

# our target
set(APP_NAME Atlas)
add_executable(${APP_NAME} WIN32 MACOSX_BUNDLE
               ${SYS_SOURCES}
               ${QTCSV_SOURCES}
               ${LIBQXT_HEADERS} ${LIBQXT_SOURCES}
               ${SOURCES} ${FORMS} ${RESOURCES} ${ASSETS}
               ${PROTO_SRCS} ${GRPC_SRCS})

target_include_directories(${APP_NAME} SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${APP_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(${APP_NAME} PRIVATE ${QTCSVDIR} ${QTCSVDIR}/include)
target_include_directories(${APP_NAME} PRIVATE ${LIBQXTDIR}/core ${LIBQXTDIR}/widgets)
target_compile_definitions(${APP_NAME} PUBLIC ATLAS_USE_CORE_PROFILE)
if (WIN32)
  target_compile_definitions(${APP_NAME} PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE WINVER=0x0601 _WIN32_WINNT=0x0601 _USE_MATH_DEFINES _SILENCE_CXX17_RESULT_OF_DEPRECATION_WARNING _SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING)

  target_compile_options(${APP_NAME} PUBLIC /GR) # Enable Run-Time Type Information
  target_compile_options(${APP_NAME} PUBLIC /EHsc) # exception handling model, https://msdn.microsoft.com/en-us/library/1deeycx5.aspx
  target_compile_options(${APP_NAME} PUBLIC /W4)
  target_compile_options(${APP_NAME} PUBLIC /bigobj) # increases the number of sections that an object file can contain.
  target_compile_options(${APP_NAME} PUBLIC /wd4267) # 'var' : conversion from 'size_t' to 'type', possible loss of data
  target_compile_options(${APP_NAME} PUBLIC /wd4244) # 'argument' : conversion from 'type1' to 'type2', possible loss of data
  target_compile_options(${APP_NAME} PUBLIC /wd4305) # 'identifier' : truncation from 'type1' to 'type2'
  target_compile_options(${APP_NAME} PUBLIC /wd4819) # The file contains a character that cannot be represented in the current code page (number). Save the file in Unicode format to prevent data loss.
  target_compile_options(${APP_NAME} PUBLIC /wd4828) # The file contains a character starting at offset 0x60 that is illegal in the current source character set (codepage 65001).
  target_compile_options(${APP_NAME} PUBLIC /wd5105) # macro expansion producing 'defined' has undefined behavior, from windows sdk
  target_compile_options(${APP_NAME} PUBLIC /utf-8)  # https://blogs.msdn.microsoft.com/vcblog/2016/02/22/new-options-for-managing-character-sets-in-the-microsoft-cc-compiler/
  if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${APP_NAME} PUBLIC /analyze) # The C/C++ Code Analysis tool provides information to developers about possible defects in their C/C++ source code
    # target_compile_options(${APP_NAME} PUBLIC /wd6255) # _alloca indicates failure by raising a stack overflow exception. Consider using _malloca instead.
    target_compile_options(${APP_NAME} PUBLIC /wd6326) # Potential comparison of a constant with another constant.
    # target_compile_options(${APP_NAME} PUBLIC /wd6330) # Incorrect type passed as parameter in call to function.
    target_compile_options(${APP_NAME} PUBLIC /wd6386) # Buffer overrun while writing to 'data'.
    target_compile_options(${APP_NAME} PUBLIC /wd28020)
    target_compile_options(${APP_NAME} PUBLIC /MP) # Build with Multiple Processes
    target_compile_options(${APP_NAME} PUBLIC /GL) # Enables whole program optimization.
  else ()
    target_compile_options(${APP_NAME} PUBLIC /MD)
  endif ()
  # standard conformance
  target_compile_options(${APP_NAME} PUBLIC /permissive- /volatile:iso /Zc:inline /Zc:preprocessor /Zc:lambda)
  target_compile_options(${APP_NAME} PUBLIC /Zc:externConstexpr /Zc:throwingNew /Zc:enumTypes /Zc:templateScope)
  target_compile_options(${APP_NAME} PUBLIC /Zc:__cplusplus) # The /Zc:__cplusplus compiler option enables the __cplusplus preprocessor macro to report an updated value for recent C++ language standards support

  #
  target_compile_options(${APP_NAME} PUBLIC /arch:AVX)

  #set_source_files_properties(${SYS_SOURCES} PROPERTIES COMPILE_FLAGS /Zc:preprocessor-)
  #set_source_files_properties(z3dnetworkevaluator.cpp PROPERTIES COMPILE_FLAGS /Zc:preprocessor-)

  if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # to create an output file whether or not LINK finds more than one definition for a symbol.
    target_link_options(${APP_NAME} PUBLIC /LTCG:STATUS /DEBUG:NONE /FORCE:MULTIPLE /VERBOSE:LIB /NODEFAULTLIB:msvcrtd.lib)
  endif ()

  # for some files
  add_custom_command(TARGET ${APP_NAME}
                     POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${APP_NAME}>/Resources")
  add_custom_command(TARGET ${APP_NAME}
                     POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/ffmpeg.exe"
                     "$<TARGET_FILE_DIR:${APP_NAME}>/Resources/")
  add_custom_command(TARGET ${APP_NAME}
                     POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/bin/h5ls.exe"
                     "$<TARGET_FILE_DIR:${APP_NAME}>/Resources/")
  add_custom_command(TARGET ${APP_NAME}
                     POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/settings_flagfile.txt"
                     "$<TARGET_FILE_DIR:${APP_NAME}>/Resources/")
  add_custom_command(TARGET ${APP_NAME}
                     POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/jre"
                     "$<TARGET_FILE_DIR:${APP_NAME}>/Resources/jre")
  add_custom_command(TARGET ${APP_NAME}
                     POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/jars"
                     "$<TARGET_FILE_DIR:${APP_NAME}>/Resources/jars")
  add_custom_command(TARGET ${APP_NAME}
                     POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/Resources/ontology
                     $<TARGET_FILE_DIR:${APP_NAME}>/Resources/ontology)
  add_custom_command(TARGET ${APP_NAME}
                     POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../neurolabi/json
                     $<TARGET_FILE_DIR:${APP_NAME}>/Resources/json)
else (WIN32)
  target_compile_options(${APP_NAME} PUBLIC -frtti)
  target_compile_options(${APP_NAME} PUBLIC -fexceptions -fwrapv -fno-strict-aliasing)
  target_compile_options(${APP_NAME} PUBLIC -Wall -Wextra)
  target_compile_options(${APP_NAME} PUBLIC -Waddress -Wdeprecated)
  if (APPLE AND AARCH64)
    target_compile_options(${APP_NAME} PUBLIC -mcpu=apple-m1)
  else ()
    target_compile_options(${APP_NAME} PUBLIC -mavx)
  endif ()
  target_compile_options(${APP_NAME} PUBLIC -fno-common)

  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${APP_NAME} PUBLIC -Wbool-conversion -Wconstant-conversion -Wenum-conversion)
    target_compile_options(${APP_NAME} PUBLIC -Waddress-of-temporary)
    target_compile_options(${APP_NAME} PUBLIC -mllvm -inline-threshold=600)
    target_compile_options(${APP_NAME} PUBLIC -Wshadow)
    target_compile_options(${APP_NAME} PUBLIC -Wno-c++98-c++11-c++14-c++17-compat)
    target_compile_options(${APP_NAME} PRIVATE -Wundefined-reinterpret-cast)
    if (APPLE)
      target_compile_options(${APP_NAME} PUBLIC -stdlib=libc++)
      target_link_options(${APP_NAME} PUBLIC -stdlib=libc++)
    else ()
      target_compile_options(${APP_NAME} PUBLIC -fsized-deallocation)
    endif ()
  elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(${APP_NAME} PUBLIC -Wl,--allow-multiple-definition)
  endif ()

  if (ATLAS_SANITIZE_ADDRESS)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
      target_compile_options(${APP_NAME} PUBLIC -fsanitize=address,undefined,integer,nullability -fno-omit-frame-pointer -fno-sanitize=unsigned-integer-overflow)
      target_link_options(${APP_NAME} PUBLIC -fsanitize=address,undefined,integer,nullability -fno-omit-frame-pointer -fno-sanitize=unsigned-integer-overflow)
    else ()
      target_compile_options(${APP_NAME} PUBLIC -fsanitize=address,undefined,integer,nullability -fno-omit-frame-pointer -fno-sanitize=unsigned-integer-overflow)
      target_link_options(${APP_NAME} PUBLIC -fsanitize=address,undefined,integer,nullability -fno-omit-frame-pointer -fno-sanitize=unsigned-integer-overflow)
    endif ()
  endif ()

  # for some files
  set_source_files_properties(${GRPC_SRCS} PROPERTIES COMPILE_FLAGS
                              "-Wno-unused-parameter")

  if (APPLE)
    message(STATUS "Qt6Core_INCLUDE_DIRS: ${Qt6Core_INCLUDE_DIRS}")
    list(GET Qt6Core_INCLUDE_DIRS 0 Qt6Core_INCLUDE_DIRS0)
    message(STATUS "Qt6Core_INCLUDE_DIRS0: ${Qt6Core_INCLUDE_DIRS0}")
    set_source_files_properties(ztheme.cpp PROPERTIES COMPILE_FLAGS "-isystem ${Qt6Core_INCLUDE_DIRS0}/Headers/${QT_VERSION}/QtCore -isystem ${Qt6Core_INCLUDE_DIRS0}/Headers/${QT_VERSION}")

    set(copyright_line "Copyright 2011-2019 Linqing Feng")
    set_target_properties(${APP_NAME} PROPERTIES
                          BUILD_RPATH "@executable_path/../Frameworks"
                          INSTALL_RPATH "@executable_path/../Frameworks"
                          OUTPUT_NAME "${APP_NAME}"
                          MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Info.plist.in"
                          MACOSX_BUNDLE_INFO_STRING "${APP_NAME}"
                          MACOSX_BUNDLE_LONG_VERSION_STRING "${ATLAS_VERSION}"
                          MACOSX_BUNDLE_SHORT_VERSION_STRING "${ATLAS_VERSION}"
                          MACOSX_BUNDLE_BUNDLE_NAME "${APP_NAME}"
                          MACOSX_BUNDLE_BUNDLE_VERSION "${ATLAS_VERSION}"
                          MACOSX_BUNDLE_COPYRIGHT "${copyright_line}"
                          MACOSX_BUNDLE_GUI_IDENTIFIER "fenglab.${APP_NAME}"
                          RESOURCE "${ASSETS}"
                          )
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/jre/Contents/Home
                       $<TARGET_FILE_DIR:${APP_NAME}>/../Resources/jre)
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/jre-arm/Contents/Home
                       $<TARGET_FILE_DIR:${APP_NAME}>/../Resources/jre-arm)
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/jars
                       $<TARGET_FILE_DIR:${APP_NAME}>/../Resources/jars)
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/Resources/ontology
                       $<TARGET_FILE_DIR:${APP_NAME}>/../Resources/ontology)
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../neurolabi/json
                       $<TARGET_FILE_DIR:${APP_NAME}>/../Resources/json)
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND codesign --force --deep --sign - $<TARGET_BUNDLE_DIR:${APP_NAME}>)
  else ()
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/Resources/Atlas.png
                       $<TARGET_FILE_DIR:${APP_NAME}>)
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${APP_NAME}>/Resources/)
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/ffmpeg
                       $<TARGET_FILE_DIR:${APP_NAME}>/Resources/)
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/bin/h5ls
                       $<TARGET_FILE_DIR:${APP_NAME}>/Resources/)
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/settings_flagfile.txt
                       $<TARGET_FILE_DIR:${APP_NAME}>/Resources/)
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/jre
                       $<TARGET_FILE_DIR:${APP_NAME}>/Resources/jre)
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/build/jars
                       $<TARGET_FILE_DIR:${APP_NAME}>/Resources/jars)
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/Resources/ontology
                       $<TARGET_FILE_DIR:${APP_NAME}>/Resources/ontology)
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../neurolabi/json
                       $<TARGET_FILE_DIR:${APP_NAME}>/Resources/json)
  endif ()
endif (WIN32)

target_link_libraries(${APP_NAME} PUBLIC glbinding::glbinding-aux glbinding::glbinding
                      img neutu ${OpenCV_LIBS} Qt6::Core Qt6::Widgets Qt6::Network Qt6::Svg Qt6::SvgWidgets Qt6::Concurrent
                      Qt6::OpenGLWidgets Qt6::CorePrivate Qt6::Test benchmark::benchmark
                      gRPC::grpc++ protobuf::libprotobuf
                      # RocksDB::rocksdb
                      )

if (UNIX AND NOT APPLE)
  target_link_libraries(${APP_NAME} PRIVATE OpenGL::OpenGL OpenGL::EGL)
endif ()
