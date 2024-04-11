cmake_minimum_required(VERSION 3.17.5)
project(neutu)

# main stuff

find_package(Qt6 ${QT_VERSION} REQUIRED COMPONENTS Core
             PATHS ${QT_HOST_PATH} NO_DEFAULT_PATH)
print_target_properties(Qt6::Core)

# main srcs
set(NEUTU_SOURCES
    swc/zswcpruner.cpp swc/zswcpruner.h
    swc/zswcconnector.cpp swc/zswcconnector.h
    swc/zswcresampler.cpp swc/zswcresampler.h
    swc/zswcsignalfitter.cpp swc/zswcsignalfitter.h
    geometry/zintcuboid.cpp geometry/zintcuboid.h
    geometry/zpointarray.cpp geometry/zpointarray.h
    geometry/zlinesegment.cpp geometry/zlinesegment.h
    geometry/zintcuboidarray.cpp geometry/zintcuboidarray.h
    geometry/zintcuboidface.cpp geometry/zintcuboidface.h
    geometry/zgeometry.cpp geometry/zgeometry.h
    geometry/zpoint.cpp geometry/zpoint.h
    geometry/zaffinerect.cpp geometry/zaffinerect.h
    geometry/zaffineplane.cpp geometry/zaffineplane.h
    geometry/zgeo3dscalarfield.cpp geometry/zgeo3dscalarfield.h
    geometry/zplane.cpp geometry/zplane.h
    geometry/zcuboid.cpp geometry/zcuboid.h
    geometry/zintpoint.cpp geometry/zintpoint.h
    geometry/zgeo3dtransform.cpp geometry/zgeo3dtransform.h
    mylib/array.cpp mylib/array.h
    mylib/mylib.cpp mylib/mylib.h
    mylib/utilities.cpp mylib/utilities.h
    filesystem/utilities.cpp filesystem/utilities.h
    imgproc/zstackbinarizer.cpp imgproc/zstackbinarizer.h
    imgproc/zstackprocessor.cpp imgproc/zstackprocessor.h
    neurolabi/zdoublevector.cpp neurolabi/zdoublevector.h
    neurolabi/numericarray.cpp neurolabi/numericarray.h
    common/memorystream.cpp common/memorystream.h
    common/utilities.cpp common/utilities.h
    common/neutudefs.cpp common/neutudefs.h
    common/math.cpp common/math.h common/mathdefs.h
    common/userinfo.cpp common/userinfo.h
    data3d/zstackobjecthandle.cpp data3d/zstackobjecthandle.h
    flyem/zflyemneuronrangecompare.cpp flyem/zflyemneuronrangecompare.h
    flyem/zflyemneuronaxis.cpp flyem/zflyemneuronaxis.h
    flyem/zflyemneuronrange.cpp flyem/zflyemneuronrange.h
    misc/miscutility.cpp misc/miscutility.h
    zneurontracer.cpp zneurontracer.h
    zneurontracerconfig.cpp zneurontracerconfig.h
    zjsonobject.cpp zjsonobject.h
    zjsonarray.cpp zjsonarray.h
    zjsonvalue.cpp zjsonvalue.h
    swctreenode.cpp swctreenode.h
    zswcforest.cpp zswcforest.h
    zjsonparser.cpp zjsonparser.h
    zstackgraph.cpp zstackgraph.h
    zvoxelarray.cpp zvoxelarray.h
    zinthistogram.cpp zinthistogram.h
    zswctree.cpp zswctree.h
    c_json.cpp c_json.h
    c_stack.cpp c_stack.h
    zstring.cpp zstring.h
    zvoxel.cpp zvoxel.h
    zfiletype.cpp zfiletype.h
    zfilelist.cpp zfilelist.h
    zswcpath.cpp zswcpath.h
    zswcbranch.cpp zswcbranch.h
    zswcfactory.cpp zswcfactory.h
    zclosedcurve.cpp zclosedcurve.h
    zstackobject.cpp zstackobject.h
    zobject3dscan.cpp zobject3dscan.h
    zprogressable.cpp zprogressable.h
    zstackfactory.cpp zstackfactory.h
    zlocalneuroseg.cpp zlocalneuroseg.h
    zobject3darray.cpp zobject3darray.h
    zweightedpoint.cpp zweightedpoint.h
    zswctreematcher.cpp zswctreematcher.h
    zrandomgenerator.cpp zrandomgenerator.h
    zjsonobjectparser.cpp zjsonobjectparser.h
    zswcfeatureanalyzer.cpp zswcfeatureanalyzer.h
    zweightedpointarray.cpp zweightedpointarray.h
    zhistogram.cpp zhistogram.h
    zhdf5reader.cpp zhdf5reader.h
    zhdf5writer.cpp zhdf5writer.h
    zresolution.cpp zresolution.h
    zeigensolver.cpp zeigensolver.h
    zstackwriter.cpp zstackwriter.h
    zspgrowparser.cpp zspgrowparser.h
    zobject3dstripe.cpp zobject3dstripe.h
    zobject3dfactory.cpp zobject3dfactory.h
    zstackobjectrole.cpp zstackobjectrole.h
    zprogressreporter.cpp zprogressreporter.h
    zswctrunkanalyzer.cpp zswctrunkanalyzer.h
    zswctreenodeselector.cpp zswctreenodeselector.h
    zswcglobalfeatureanalyzer.cpp zswcglobalfeatureanalyzer.h
    zswcdirectionfeatureanalyzer.cpp zswcdirectionfeatureanalyzer.h
    zswclayershollfeatureanalyzer.cpp zswclayershollfeatureanalyzer.h
    zswcnodebufferfeatureanalyzer.cpp zswcnodebufferfeatureanalyzer.h
    neutubeconfig.cpp neutubeconfig.h
    zswctreenodearray.cpp zswctreenodearray.h
    zobject3dscanarray.cpp zobject3dscanarray.h
    zstackskeletonizer.cpp zstackskeletonizer.h
    ztextlinecompositer.cpp ztextlinecompositer.h
    zswcdisttrunkanalyzer.cpp zswcdisttrunkanalyzer.h
    zswclayertrunkanalyzer.cpp zswclayertrunkanalyzer.h
    zswcgenerator.cpp zswcgenerator.h
    zlogmessagereporter.cpp zlogmessagereporter.h
    zmessagereporter.cpp zmessagereporter.h
    zgraph.cpp zgraph.h
    zstack.cxx zstack.hxx
    zstackfile.cpp zstackfile.h
    zsinglechannelstack.cpp zsinglechannelstack.h
    zmatrix.cpp zmatrix.h
    zinttree.cpp zinttree.h
    zobject3d.cpp zobject3d.h
    zstackarray.cpp zstackarray.h
    zarray.cpp zarray.h
    zstackutil.cpp zstackutil.h
    zstackptr.cpp zstackptr.h
    zstackreader.cpp zstackreader.h
)

message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")

# our target
if (BUILD_WITH_CONDA)
  set(LIBRARY_NAME condaneutu)
else ()
  set(LIBRARY_NAME neutu)
endif ()
add_library(${LIBRARY_NAME} STATIC ${NEUTU_SOURCES})

target_include_directories(${LIBRARY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# target_compile_definitions(${LIBRARY_NAME} PUBLIC _QT_GUI_USED_)
if (WIN32)
  target_compile_options(${LIBRARY_NAME} PRIVATE /wd4065 /wd4101 /wd4245 /wd4805 /wd4456 /wd5246 /wd5262 /wd5264)
  target_compile_options(${LIBRARY_NAME} PRIVATE /wd6011 /wd28182 /wd6387 /wd6385 /wd6308
                          /wd6054 /wd6340 /wd6246 /wd6297)
  set_source_files_properties(mylib/array.cpp PROPERTIES COMPILE_FLAGS "/wd4774")
else (WIN32)
  target_compile_options(${LIBRARY_NAME} PUBLIC -Wall -Wextra) # has to come before the following -W options
  target_compile_options(${LIBRARY_NAME} PRIVATE -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-format )
  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${LIBRARY_NAME} PRIVATE -Wno-unknown-warning-option -Wundefined-reinterpret-cast)

    set_source_files_properties(mylib/array.cpp PROPERTIES COMPILE_FLAGS "-Wno-shadow -Wno-misleading-indentation")
  elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${LIBRARY_NAME} PRIVATE -Wno-implicit-fallthrough -Wno-stringop-truncation
                           -Wno-maybe-uninitialized -Wno-unused-result -Wno-misleading-indentation)
  endif ()
endif (WIN32)

target_link_libraries(${LIBRARY_NAME} PUBLIC neurolabi img glm::glm Qt6::Core)
