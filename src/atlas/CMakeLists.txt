cmake_minimum_required(VERSION 3.17.5)
project(Atlas)
set(ATLAS_VERSION ${GIT_VERSION})

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

find_package(Qt6 ${QT_VERSION} REQUIRED COMPONENTS Core Widgets Network Svg SvgWidgets Concurrent Test
             PATHS ${QT_HOST_PATH} NO_DEFAULT_PATH)
print_target_properties(Qt6::Core)
print_target_properties(Qt6::Widgets)
print_target_properties(Qt6::Network)
print_target_properties(Qt6::Svg)
print_target_properties(Qt6::SvgWidgets)
print_target_properties(Qt6::Concurrent)
print_target_properties(Qt6::Test)

if (ATLAS_USE_OPENGLWIDGET)
  find_package(Qt6 ${QT_VERSION} REQUIRED COMPONENTS OpenGLWidgets
               PATHS ${QT_HOST_PATH} NO_DEFAULT_PATH)
  print_target_properties(Qt6::OpenGLWidgets)
endif ()

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

message(STATUS "ENV{VULKAN_SDK}: $ENV{VULKAN_SDK}")
find_package(Vulkan REQUIRED COMPONENTS glslc glslangValidator)
print_target_properties(Vulkan::glslc)

# Vulkan shader offline build (SPIR-V)
set(ATLAS_VULKAN_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Resources/shader/vulkan)
set(ATLAS_VULKAN_SHADER_OUT ${ATLAS_VULKAN_SHADER_DIR}/spv)

set(ATLAS_VK_SHADER_SOURCES
    # Core
    almag.vert
    almag.frag
    background.frag
    line.vert
    line.frag
    sphere.vert
    sphere.frag
    mesh.vert
    mesh.frag
    pass.vert
    transform_with_2dtexture.vert
    pass_with_2dtexture.vert

    # 2D single-channel
    image2d_with_colormap_single_channel.frag
    image2d_with_transfun_single_channel.frag

    # Copy
    copyimage.frag
    glow.frag
    blur.frag

    # Compute
    block_id_compact.comp
    block_id_compact_append.comp

    # 3D slice and transforms
    transform_with_3dtexture.vert
    transform_with_3dtexture_and_eye_coordinate.vert
    image3d_slice_with_colormap.frag
    image3d_slice_with_transfun.frag
    image3d_slice_with_transfun_blockID.frag
    render_3dtexture_coordinate.frag
    render_3dtexture_coordinate_and_eye_coordinate.frag

    # 3D raycasters
    image3d_raycaster.frag
    image3d_raycaster_blockID.frag

    # Volume single-channel
    volume_slice_with_colormap_single_channel.frag
    volume_slice_with_transfun_single_channel.frag
    volume_raycaster_single_channel.frag

    # Ellipsoid
    ellipsoid.vert
    ellipsoid.frag

    # Cone
    cone.vert
    cone_2.vert
    cone.frag
    cone_2.frag

    # Compositors and copy
    compositor.frag
    image2d_array_compositor.frag
    copy_raycaster_image.frag
    depth_ramp.frag

    # Wide line (non-GS)
    wideline1.vert
    wideline1.frag

    # Wide line (GS-based)
    wideline.vert
    wideline.geom
    wideline.frag

    # Dual depth peeling (final passes)
    dual_peeling_blend.frag
    dual_peeling_final.frag
    # Dual depth peeling (shape init/peel)
    dual_peeling_init_cone.frag
    dual_peeling_peel_cone.frag
    dual_peeling_init_cone_2.frag
    dual_peeling_peel_cone_2.frag
    dual_peeling_init_sphere.frag
    dual_peeling_peel_sphere.frag
    dual_peeling_init_ellipsoid.frag
    dual_peeling_peel_ellipsoid.frag
    dual_peeling_init_mesh.frag
    dual_peeling_peel_mesh.frag
    dual_peeling_init_wideline.frag
    dual_peeling_peel_wideline.frag
    # Dual depth peeling (image init/peel)
    dual_peeling_init_image.frag
    dual_peeling_peel_image.frag

    # Weighted OIT (final passes)
    wavg_final.frag
    wblended_final.frag
    # Weighted OIT (shape init passes)
    wavg_init_cone.frag
    wavg_init_cone_2.frag
    wavg_init_sphere.frag
    wavg_init_ellipsoid.frag
    wavg_init_mesh.frag
    wavg_init_wideline.frag
    # Weighted OIT (image init)
    wavg_init_image.frag
    wblended_init_cone.frag
    wblended_init_cone_2.frag
    wblended_init_sphere.frag
    wblended_init_ellipsoid.frag
    wblended_init_mesh.frag
    wblended_init_wideline.frag
    # Weighted OIT (image init)
    wavg_init_image.frag
    wblended_init_image.frag
)

set(_GLSLC_EXE "")
if (TARGET Vulkan::glslc)
  get_target_property(_GLSLC_EXE Vulkan::glslc IMPORTED_LOCATION)
endif ()
if (NOT _GLSLC_EXE)
  if (DEFINED Vulkan_GLSLC_EXECUTABLE)
    set(_GLSLC_EXE ${Vulkan_GLSLC_EXECUTABLE})
  elseif (DEFINED VULKAN_GLSLC_EXECUTABLE)
    set(_GLSLC_EXE ${VULKAN_GLSLC_EXECUTABLE})
  else ()
    set(_GLSLC_EXE glslc)
  endif ()
endif ()

set(ATLAS_VK_SPV_OUTPUTS)
# Track GLSL include changes to trigger SPIR-V rebuilds
file(GLOB ATLAS_VK_SHADER_INCLUDES ${ATLAS_VULKAN_SHADER_DIR}/include/*.glslinc)
foreach(SRC ${ATLAS_VK_SHADER_SOURCES})
  set(OUT ${ATLAS_VULKAN_SHADER_OUT}/${SRC}.spv)
  add_custom_command(
          OUTPUT ${OUT}
          COMMAND ${CMAKE_COMMAND} -E make_directory ${ATLAS_VULKAN_SHADER_OUT}
          COMMAND ${_GLSLC_EXE}
          -O
          -I ${CMAKE_CURRENT_SOURCE_DIR}/Resources/shader
          -o ${OUT}
          ${ATLAS_VULKAN_SHADER_DIR}/${SRC}
          DEPENDS ${ATLAS_VULKAN_SHADER_DIR}/${SRC} ${ATLAS_VK_SHADER_INCLUDES}
          COMMENT "[glslc] ${SRC} -> ${OUT}"
          VERBATIM)
  list(APPEND ATLAS_VK_SPV_OUTPUTS ${OUT})
endforeach()

add_custom_target(vulkan_shaders ALL DEPENDS ${ATLAS_VK_SPV_OUTPUTS})

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
    zimgdisplay.h zimgdisplay.cpp
    zsectionsregistrationdialog.h zsectionsregistrationdialog.cpp
    zstitchimagedialog.h zstitchimagedialog.cpp
    zpunctadetectiondialog.h zpunctadetectiondialog.cpp
    zgenerateanalysistextfile.h zgenerateanalysistextfile.cpp
    zanalysisworklistdialog.h zanalysisworklistdialog.cpp
    zanalysisworklistmodel.h zanalysisworklistmodel.cpp
    zstyleditemdelegate.h zstyleditemdelegate.cpp
    zitemeditorfactory.h zitemeditorfactory.cpp
    zmainwindow.h zmainwindow.cpp
    zimgdoc.h zimgdoc.cpp
    zimgview.h zimgview.cpp
    zspinboxwithscrollbar.h zspinboxwithscrollbar.cpp
    zgraphicsview.h zgraphicsview.cpp
    zview.h zview.cpp
    zdoc.h zdoc.cpp
    zobjdoc.h zobjdoc.cpp
    zobjview.h zobjview.cpp
    zimgfilter.h zimgfilter.cpp
    zobjmodel.h zobjmodel.cpp
    zobjwidget.h zobjwidget.cpp
    zshareitemselectionmodel.h zshareitemselectionmodel.cpp
    zitemselectionmodel.h zitemselectionmodel.cpp
    zmessageboxhelpers.h zmessageboxhelpers.cpp
    zviewsettingwidget.h zviewsettingwidget.cpp
    z3dmainwindow.h z3dmainwindow.cpp
    z3drenderingengine.h z3drenderingengine.cpp
    z3dobjview.h z3dobjview.cpp
    z3dimgview.h z3dimgview.cpp
    zviewsettinginterface.h
    zpunctadoc.h zpunctadoc.cpp
    z3dpunctaview.h z3dpunctaview.cpp
    z3dpunctafilter.h z3dpunctafilter.cpp
    z3dmeshfilter.h z3dmeshfilter.cpp
    z3dswcfilter.h z3dswcfilter.cpp
    zmeshdoc.h zmeshdoc.cpp
    zswcdoc.h zswcdoc.cpp
    z3dswcview.h z3dswcview.cpp
    z3dmeshview.h z3dmeshview.cpp
    zobjfilter.h zobjfilter.cpp
    zanimation.h zanimation.cpp
    zparameteranimation.h zparameteranimation.cpp
    zparameterkey.h zparameterkey.cpp
    zcameraparameterkey.h zcameraparameterkey.cpp
    zcameraparameteranimation.h zcameraparameteranimation.cpp
    z3danimation.h z3danimation.cpp
    z3danimationdoc.h z3danimationdoc.cpp
    zsaveobjsdialog.h zsaveobjsdialog.cpp
    zanimationwidget.h zanimationwidget.cpp
    zobjeditwidget.h zobjeditwidget.cpp
    zparameterfactory.h zparameterfactory.cpp
    zfontparameter.h zfontparameter.cpp
    zfontwidget.h zfontwidget.cpp
    z2danimation.h z2danimation.cpp
    z2danimationdoc.h z2danimationdoc.cpp
    ztimelinewidget.h ztimelinewidget.cpp
    ztimelineobjview.h ztimelineobjview.cpp
    ztimelineeventview.h ztimelineeventview.cpp
    ztimelineobjscene.h ztimelineobjscene.cpp
    ztimelineeventscene.h ztimelineeventscene.cpp
    ztimelineaxisview.h ztimelineaxisview.cpp
    ztimelinekeyeditdialog.h ztimelinekeyeditdialog.cpp
    zdockwidget.h zdockwidget.cpp
    z3danimationview.h z3danimationview.cpp
    z3danimationfilter.h z3danimationfilter.cpp
    z3dcompositor.h z3dcompositor.cpp
    z3dgeometryfilter.h z3dgeometryfilter.cpp
    z3dboundedfilter.h z3dboundedfilter.cpp
    z3dmeshrenderer.h z3dmeshrenderer.cpp
    z3dtexturecopyrenderer.h z3dtexturecopyrenderer.cpp
    z3dtexturecoordinaterenderer.h z3dtexturecoordinaterenderer.cpp
    z3dtextureblendrenderer.h z3dtextureblendrenderer.cpp
    z3dsphererenderer.h z3dsphererenderer.cpp
    z3dlinewithfixedwidthcolorrenderer.h z3dlinewithfixedwidthcolorrenderer.cpp
    z3dlinerenderer.h z3dlinerenderer.cpp
    z3dfontrenderer.h z3dfontrenderer.cpp
    z3dellipsoidrenderer.h z3dellipsoidrenderer.cpp
    z3dconerenderer.h z3dconerenderer.cpp
    z3dbackgroundrenderer.h z3dbackgroundrenderer.cpp
    z3darrowrenderer.h z3darrowrenderer.cpp
    z3drendererbase.h z3drendererbase.cpp
    z3drendererbackend.h z3drendererglbackend.cpp
    z3drenderervulkanbackend.h z3drenderervulkanbackend.cpp
    z3drenderglobalstate.h z3drenderglobalstate.cpp
    z3dperfcollector.h z3dperfcollector.cpp
    z3dprimitiverenderer.h z3dprimitiverenderer.cpp
    z3dcanvaspainter.h z3dcanvaspainter.cpp
    z3dnetworkevaluator.h z3dnetworkevaluator.cpp
    z3dcanvas.h z3dcanvas.cpp
    z3dscene.h z3dscene.cpp
    z3dshadergroup.h z3dshadergroup.cpp
    z3dport.h z3dport.cpp
    z3dglobalparameters.h z3dglobalparameters.cpp
    z3dscratchresourcepool.h z3dscratchresourcepool.cpp
    z3dshaderprogram.h z3dshaderprogram.cpp
    z3dtransferfunction.h z3dtransferfunction.cpp
    z3dtransferfunctioneditor.h z3dtransferfunctioneditor.cpp
    z3dtransferfunctionwidgetwitheditorwindow.h z3dtransferfunctionwidgetwitheditorwindow.cpp
    zclickablelabel.h zclickablelabel.cpp
    zcolormap.h zcolormap.cpp
    zcolormapeditor.h zcolormapeditor.cpp
    zcolormapwidgetwitheditorwindow.h zcolormapwidgetwitheditorwindow.cpp
    z3dvertexarrayobject.h z3dvertexarrayobject.cpp
    z3dvertexbufferobject.h z3dvertexbufferobject.cpp
    z3dpickingmanager.h z3dpickingmanager.cpp
    z3dfilterview.h
    zfilterview.h
    zboostgeometryadaptor.h
    z3dcanvaseventlistener.h
    zimgvoxelcolormap.h
    zroi.h zroi.cpp
    zroidoc.h zroidoc.cpp
    zroiview.h zroiview.cpp
    zroifilter.h zroifilter.cpp
    zquatparameter.h zquatparameter.cpp
    z3dshadermanager.h z3dshadermanager.cpp
    zanimationexportwidget.h zanimationexportwidget.cpp
    zvideoencoder.h zvideoencoder.cpp
    zswcfilter.h zswcfilter.cpp
    zswcview.h zswcview.cpp
    zpunctafilter.h zpunctafilter.cpp
    zpunctaview.h zpunctaview.cpp
    znumericparameter.h znumericparameter.cpp
    zoptionparameter.h zoptionparameter.cpp
    zparameter.h zparameter.cpp
    zcombobox.h zcombobox.cpp
    zselectfilewidget.h zselectfilewidget.cpp
    zloadimagesequencedialog.h zloadimagesequencedialog.cpp
    zspinbox.h zspinbox.cpp
    zspinboxwithslider.h zspinboxwithslider.cpp
    zwidgetsgroup.h zwidgetsgroup.cpp
    z3dcamera.h z3dcamera.cpp
    z3dcameraparameter.h z3dcameraparameter.cpp
    z3drendertarget.h z3drendertarget.cpp
    z3dinteractionhandler.h z3dinteractionhandler.cpp
    zeventlistenerparameter.h zeventlistenerparameter.cpp
    z3dtexture.h z3dtexture.cpp
    z3dtransformparameter.h z3dtransformparameter.cpp
    z3dgpuinfo.h z3dgpuinfo.cpp
    zstringparameter.h zstringparameter.cpp
    z3dsdfont.h z3dsdfont.cpp
    zspanslider.h zspanslider.cpp
    zcustomcommand.h zcustomcommand.cpp
    ztakescreenshotwidget.h ztakescreenshotwidget.cpp
    z3dshader.h z3dshader.cpp
    z3dcontext.h z3dcontext.cpp
    z3dgl.h z3dgl.cpp
    zimgpack.h zimgpack.cpp
    zimgpackdisplay.h zimgpackdisplay.cpp
    zimgcache.h zimgcache.cpp
    zsysteminfo.h zsysteminfo.cpp
    zfileutils.h zfileutils.cpp
    zregionannotation.h zregionannotation.cpp
    zregionannotationdoc.h zregionannotationdoc.cpp
    z3dregionannotationview.h z3dregionannotationview.cpp
    zregionannotationview.h zregionannotationview.cpp
    zregionannotationfilter.h zregionannotationfilter.cpp
    z3dregionannotationfilter.h z3dregionannotationfilter.cpp
    zregionannotationwidget.h zregionannotationwidget.cpp
    zregionannotationtreemodel.h zregionannotationtreemodel.cpp
    zregionannotationtreeview.h zregionannotationtreeview.cpp
    zbuttoncolumndelegate.h zbuttoncolumndelegate.cpp
    zchooseobjdialog.h zchooseobjdialog.cpp
    zgraphicsscene.h zgraphicsscene.cpp
    z3dfilter.h z3dfilter.cpp
    z3dtextureglowrenderer.h z3dtextureglowrenderer.cpp
    z3dcameracontrolwidget.h z3dcameracontrolwidget.cpp
    z3dtextureandeyecoordinaterenderer.h z3dtextureandeyecoordinaterenderer.cpp
    z3dimgraycasterrenderer.h z3dimgraycasterrenderer.cpp
    z3dimg.h z3dimg.cpp
    z3dimgfilter.h z3dimgfilter.cpp
    z3dimgslicerenderer.h z3dimgslicerenderer.cpp
    zregionontology.h zregionontology.cpp
    zobjdetailedinfowidget.h zobjdetailedinfowidget.cpp
    zlogdialog.h zlogdialog.cpp
    zlogmodelsink.h zlogmodelsink.cpp
    main.cpp
    z3dblockcache.h
    zapplication.cpp zapplication.h
    zsvgdoc.cpp zsvgdoc.h
    zsvgfilter.cpp zsvgfilter.h
    zsvgview.cpp zsvgview.h
    zgraphicsitemtype.h
    zparametereditdialog.cpp zparametereditdialog.h
    z2dtransformparameter.cpp z2dtransformparameter.h
    zspinboxdelegate.cpp zspinboxdelegate.h
    z3dopenglwidget.cpp z3dopenglwidget.h
    zchromaticshiftcorrectiondialog.cpp zchromaticshiftcorrectiondialog.h
    zlogwidget.cpp zlogwidget.h
    zimgprocessdialog.cpp zimgprocessdialog.h
    zservicemanager.cpp zservicemanager.h
    zrpcservice.cpp zrpcservice.h
    ztheme.cpp ztheme.h
    zserializationutils.h
    zroiwidget.cpp zroiwidget.h
    zgraphicsitemgroup.cpp zgraphicsitemgroup.h
    zchooseregiondialog.cpp zchooseregiondialog.h
    zregionannotationviewsettingtreemodel.h zregionannotationviewsettingtreemodel.cpp
    zregionannotationviewsettingtreeview.h zregionannotationviewsettingtreeview.cpp
    zregionannotationviewsettingcolumndelegate.h zregionannotationviewsettingcolumndelegate.cpp
    zregionviewsettingwitheditorwindow.h zregionviewsettingwitheditorwindow.cpp
    zpunctawidget.cpp zpunctawidget.h
    zpunctatablemodel.h zpunctatablemodel.cpp
    zpunctatableview.cpp zpunctatableview.h
    zpunctapack.cpp zpunctapack.h
    zswcpack.cpp zswcpack.h
    zswcwidget.cpp zswcwidget.h
    zswctreeview.cpp zswctreeview.h
    zswctreemodel.cpp zswctreemodel.h
    zobjpack.cpp zobjpack.h
    zroipack.cpp zroipack.h
    zregionannotationpack.cpp zregionannotationpack.h
    zswccolorparameters.cpp zswccolorparameters.h
    zthreadsafelrucache.h zthreadsafescalablecache.h
    zimgregioncache.cpp zimgregioncache.h
    zcheckbox.cpp zcheckbox.h
    zrunexport3danimation.cpp zrunexport3danimation.h
    zrunneutucommand.cpp zrunneutucommand.h
    zprocess.cpp zprocess.h
    zvulkan.h
    zvulkancontext.h
    zvulkancontext.cpp
    zvulkandevice.h
    zvulkandevice.cpp
    zvulkanframeexecutor.h
    zvulkanframeexecutor.cpp
    zvulkanbuffer.h
    zvulkanbuffer.cpp
    zvulkantexture.h
    zvulkantexture.cpp
    zvulkandescriptorpool.h
    zvulkandescriptorpool.cpp
    zvulkandescriptorset.h
    zvulkandescriptorset.cpp
    zvulkan.cpp
    zvulkan_vma.cpp
    zvulkanshader.h
    zvulkanshader.cpp
    zvulkanpipeline.h
    zvulkanpipeline.cpp
    zvulkanpipelinecontext_raii.h
    zvulkanpipelinecontext_raii.cpp
    zvulkanbindings.h
    zvulkanrenderconversions.h
    zvulkanrenderconversions.cpp
    zvulkanuniforms.h
    z3dtypes.h
    zvulkanshaderstate.h
    zvulkanlinepipelinecontext.h
    zvulkanlinepipelinecontext.cpp
    zvulkanmeshpipelinecontext.h
    zvulkanmeshpipelinecontext.cpp
    zvulkanellipsoidpipelinecontext.h
    zvulkanellipsoidpipelinecontext.cpp
    zvulkanspherepipelinecontext.h
    zvulkanspherepipelinecontext.cpp
    zvulkanbackgroundpipelinecontext.h
    zvulkanbackgroundpipelinecontext.cpp
    zvulkantexturecopypipelinecontext.h
    zvulkantexturecopypipelinecontext.cpp
    zvulkantextureblendpipelinecontext.h
    zvulkantextureblendpipelinecontext.cpp
    zvulkantexturedualpeelpipelinecontext.h
    zvulkantexturedualpeelpipelinecontext.cpp
    zvulkantextureweightedaveragepipelinecontext.h
    zvulkantextureweightedaveragepipelinecontext.cpp
    zvulkantextureweightedblendedpipelinecontext.h
    zvulkantextureweightedblendedpipelinecontext.cpp
    zvulkantextureglowpipelinecontext.h
    zvulkantextureglowpipelinecontext.cpp
    zvulkanimgslicepipelinecontext.h
    zvulkanimgslicepipelinecontext.cpp
    zvulkanimgraycasterpipelinecontext.h
    zvulkanimgraycasterpipelinecontext.cpp
    zvulkanfontpipelinecontext.h
    zvulkanfontpipelinecontext.cpp
    zvulkanimageblockuploader.cpp
    zvulkanpagedimageblockuploader.h
    zvulkanpagedimageblockuploader.cpp
    zvulkanconepipelinecontext.h
    zvulkanconepipelinecontext.cpp
)

# our target
set(APP_NAME Atlas)
add_executable(${APP_NAME} WIN32 MACOSX_BUNDLE
               ${SYS_SOURCES}
               ${QTCSV_SOURCES}
               ${LIBQXT_HEADERS} ${LIBQXT_SOURCES}
               ${SOURCES} ${FORMS} ${RESOURCES} ${ASSETS}
               ${PROTO_SRCS} ${GRPC_SRCS})

add_dependencies(${APP_NAME} vulkan_shaders)

target_include_directories(${APP_NAME} SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${APP_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(${APP_NAME} PRIVATE ${QTCSVDIR} ${QTCSVDIR}/include)
target_include_directories(${APP_NAME} PRIVATE ${LIBQXTDIR}/core ${LIBQXTDIR}/widgets)
if (WIN32)
  target_include_directories(${APP_NAME} PRIVATE $ENV{VULKAN_SDK}/Include)
else ()
  target_include_directories(${APP_NAME} PRIVATE $ENV{VULKAN_SDK}/include) # do not use Vulkan::Headers, which might point to /usr/include
endif ()
target_compile_definitions(${APP_NAME} PUBLIC ATLAS_USE_CORE_PROFILE)
if (WIN32)
  set_source_files_properties(${LIBQXTDIR}/core/qxtcsvmodel.cpp PROPERTIES COMPILE_FLAGS /wd4458) # declaration of 'd_ptr' hides class member
  set_source_files_properties(${GRPC_SRCS} PROPERTIES COMPILE_FLAGS "/wd4946") # reinterpret_cast used between related classes: 'class1' and 'class2'
  set_source_files_properties(${PROTO_SRCS} PROPERTIES COMPILE_FLAGS "/wd4946") # reinterpret_cast used between related classes: 'class1' and 'class2'

  if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # to create an output file whether or not LINK finds more than one definition for a symbol.
    target_link_options(${APP_NAME} PUBLIC /LTCG:STATUS /DEBUG:NONE /FORCE:MULTIPLE /VERBOSE:LIB /NODEFAULTLIB:msvcrtd.lib)
  endif ()

  # add manifest
  target_sources(${APP_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Resources/app.manifest")

  # post process
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
  add_custom_command(TARGET ${APP_NAME} POST_BUILD
                     # Copy Vulkan resources
                     COMMAND ${CMAKE_COMMAND} -E copy_if_different
                     $ENV{VULKAN_SDK}/Bin/VkLayer_khronos_profiles.dll
                     $<TARGET_FILE_DIR:${APP_NAME}>/vulkan/VkLayer_khronos_profiles.dll

                     COMMAND ${CMAKE_COMMAND} -E copy_if_different
                     $ENV{VULKAN_SDK}/Bin/VkLayer_khronos_profiles.json
                     $<TARGET_FILE_DIR:${APP_NAME}>/vulkan/VkLayer_khronos_profiles.json

                     COMMAND ${CMAKE_COMMAND} -E copy_if_different
                     $ENV{VULKAN_SDK}/Bin/VkLayer_khronos_validation.dll
                     $<TARGET_FILE_DIR:${APP_NAME}>/vulkan/VkLayer_khronos_validation.dll

                     COMMAND ${CMAKE_COMMAND} -E copy_if_different
                     $ENV{VULKAN_SDK}/Bin/VkLayer_khronos_validation.json
                     $<TARGET_FILE_DIR:${APP_NAME}>/vulkan/VkLayer_khronos_validation.json
  )
else (WIN32)
  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${APP_NAME} PRIVATE -Wundefined-reinterpret-cast)
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
  set_source_files_properties(${GRPC_SRCS} PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter")
  set_source_files_properties(zvulkan_vma.cpp PROPERTIES COMPILE_FLAGS "-Wno-nullability-completeness -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-private-field")

  if (APPLE)
    message(STATUS "Qt6Core_INCLUDE_DIRS: ${Qt6Core_INCLUDE_DIRS}")
    list(GET Qt6Core_INCLUDE_DIRS 0 Qt6Core_INCLUDE_DIRS0)
    message(STATUS "Qt6Core_INCLUDE_DIRS0: ${Qt6Core_INCLUDE_DIRS0}")
    set_source_files_properties(ztheme.cpp PROPERTIES COMPILE_FLAGS "-isystem ${Qt6Core_INCLUDE_DIRS0}/Headers/${QT_VERSION}/QtCore -isystem ${Qt6Core_INCLUDE_DIRS0}/Headers/${QT_VERSION}")

    set(copyright_line "Copyright 2011-2019 Linqing Feng")
    set_target_properties(${APP_NAME} PROPERTIES
                          BUILD_RPATH "@executable_path/../Frameworks;@executable_path/../lib"
                          INSTALL_RPATH "@executable_path/../Frameworks;@executable_path/../lib"
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
                       COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${APP_NAME}>/../Resources/shader/vulkan/spv
                       COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/Resources/shader/vulkan/spv
                       $<TARGET_FILE_DIR:${APP_NAME}>/../Resources/shader/vulkan/spv
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
    add_custom_command(TARGET ${APP_NAME} POST_BUILD
                       # Copy Vulkan resources
                       COMMAND ${CMAKE_COMMAND} -E copy_directory
                       $ENV{VULKAN_SDK}/share/vulkan
                       $<TARGET_FILE_DIR:${APP_NAME}>/../Resources/vulkan

                       # Copy libvulkan.1.dylib if it exists
                       COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       $ENV{VULKAN_SDK}/lib/libvulkan.1.dylib
                       $<TARGET_FILE_DIR:${APP_NAME}>/../lib/libvulkan.1.dylib

                       # Create symlink for libvulkan.dylib if libvulkan.1.dylib exists
                       COMMAND ${CMAKE_COMMAND} -E create_symlink
                       libvulkan.1.dylib
                       $<TARGET_FILE_DIR:${APP_NAME}>/../lib/libvulkan.dylib

                       # Copy libMoltenVK.dylib if it exists
                       COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       $ENV{VULKAN_SDK}/lib/libMoltenVK.dylib
                       $<TARGET_FILE_DIR:${APP_NAME}>/../lib/libMoltenVK.dylib

                       COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       $ENV{VULKAN_SDK}/lib/libVkLayer_khronos_profiles.dylib
                       $<TARGET_FILE_DIR:${APP_NAME}>/../lib/libVkLayer_khronos_profiles.dylib

                       COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       $ENV{VULKAN_SDK}/lib/libVkLayer_khronos_validation.dylib
                       $<TARGET_FILE_DIR:${APP_NAME}>/../lib/libVkLayer_khronos_validation.dylib
    )
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND codesign --force --deep --sign - $<TARGET_BUNDLE_DIR:${APP_NAME}>)
  else ()
    add_custom_command(TARGET ${APP_NAME}
                       POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${APP_NAME}>/Resources/shader/vulkan/spv
                       COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/Resources/shader/vulkan/spv
                       $<TARGET_FILE_DIR:${APP_NAME}>/Resources/shader/vulkan/spv
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
    add_custom_command(TARGET ${APP_NAME} POST_BUILD
                       # Copy Vulkan resources
                       COMMAND ${CMAKE_COMMAND} -E copy_directory
                       $ENV{VULKAN_SDK}/share/vulkan
                       $<TARGET_FILE_DIR:${APP_NAME}>/Resources/vulkan

                       COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       $ENV{VULKAN_SDK}/lib/libVkLayer_khronos_profiles.so
                       $<TARGET_FILE_DIR:${APP_NAME}>/lib/libVkLayer_khronos_profiles.so

                       COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       $ENV{VULKAN_SDK}/lib/libVkLayer_khronos_validation.so
                       $<TARGET_FILE_DIR:${APP_NAME}>/lib/libVkLayer_khronos_validation.so
    )
  endif ()
endif (WIN32)

target_link_libraries(${APP_NAME} PUBLIC img neutu
                      glbinding::glbinding-aux glbinding::glbinding
                      ${OpenCV_LIBS}
                      Qt6::Core Qt6::Widgets Qt6::Network Qt6::Svg Qt6::SvgWidgets Qt6::Concurrent
                      Qt6::CorePrivate Qt6::Test
                      gRPC::grpc++ protobuf::libprotobuf
                      # RocksDB::rocksdb
)

if (UNIX AND NOT APPLE)
  target_link_libraries(${APP_NAME} PRIVATE OpenGL::OpenGL OpenGL::EGL)
endif ()

if (ATLAS_USE_OPENGLWIDGET)
  target_link_libraries(${APP_NAME} PRIVATE Qt6::OpenGLWidgets)
endif ()
