cmake_minimum_required(VERSION 3.17.5)
project(neutube)

find_package(Qt6 ${QT_VERSION} REQUIRED COMPONENTS Core
             PATHS ${QT_HOST_PATH} NO_DEFAULT_PATH)
print_target_properties(Qt6::Core)

set(NEUTUBE_SOURCES
    zneutubedarrayqsort.cpp zneutubedarrayqsort.h
    zneutubedarraymath.cpp zneutubedarraymath.h
    zneutubecontfun.h
    zneutube3dgeom.cpp zneutube3dgeom.h
    zneutubecompareswc.cpp zneutubecompareswc.h
    zneutubegeneral.cpp zneutubegeneral.h
    zneutubeneurocompconn.cpp zneutubeneurocompconn.h
    zneutubegeo3dpointarray.cpp zneutubegeo3dpointarray.h
    zneutubegeo3dcircle.h
    zneutubegeo3dellipse.cpp zneutubegeo3dellipse.h
    zneutubegeo3dscalarfield.cpp zneutubegeo3dscalarfield.h
    zneutubegeo3dutils.cpp zneutubegeo3dutils.h
    zneutubeedt3d.cpp zneutubeedt3d.h
    zneutubegraph.cpp zneutubegraph.h
    zneutubeinthistogram.cpp zneutubeinthistogram.h
    zneutubeimgbwmorph.cpp zneutubeimgbwmorph.h
    zneutubeimgbinarizer.cpp zneutubeimgbinarizer.h
    zneutubeimglocmax.cpp zneutubeimglocmax.h
    zneutubeimgsampling.cpp zneutubeimgsampling.h
    zneutubeneighborhood.cpp zneutubeneighborhood.h
    zneutubeneuroseg.cpp zneutubeneuroseg.h
    zneutubeneuronstructure.cpp zneutubeneuronstructure.h
    zneutubeobjlabel.cpp zneutubeobjlabel.h
    zneutubeoptimizeutils.cpp zneutubeoptimizeutils.h
    zneutubeperceptor.cpp zneutubeperceptor.h
    zneutubeplanaredt.cpp zneutubeplanaredt.h
    zneutubespgrow.cpp zneutubespgrow.h
    zneutubespgrowparser.cpp zneutubespgrowparser.h
    zneutubetrace.cpp zneutubetrace.h
    zneutubetraceinteractive.cpp zneutubetraceinteractive.h
    zneutubetraceauto.cpp zneutubetraceauto.h
    zneutubetraceconfig.cpp zneutubetraceconfig.h
    zneutubeautotraceprocess.cpp zneutubeautotraceprocess.h
    zneutubeskeletonizeprocess.cpp zneutubeskeletonizeprocess.h
    zneutubetraceconnectiontestworkspace.cpp zneutubetraceconnectiontestworkspace.h
    zneutubetraceconnectiontest.cpp zneutubetraceconnectiontest.h
    zneutubetracedefs.h
    zneutubetracerecord.cpp zneutubetracerecord.h
    zneutubetracemask.cpp zneutubetracemask.h
    zneutubetraceseed.cpp zneutubetraceseed.h
    zneutubetraceseeder.cpp zneutubetraceseeder.h
    zneutubetraceallseeds.cpp zneutubetraceallseeds.h
    zneutubetracescorethresholds.cpp zneutubetracescorethresholds.h
    zneutubetracerecover.cpp zneutubetracerecover.h
    zneutubetracechainscreen.cpp zneutubetracechainscreen.h
    zneutubetracelocseglabel.cpp zneutubetracelocseglabel.h
    zneutubestackfitoptions.h
    zneutubestackfitscore.cpp zneutubestackfitscore.h
    zneutubestackgraph.cpp zneutubestackgraph.h
    zneutubestackroute.cpp zneutubestackroute.h
    zneutubetraceworkspace.cpp zneutubetraceworkspace.h
    zneutubeskeletonize.cpp zneutubeskeletonize.h
    zneutubeskeletonizer.cpp zneutubeskeletonizer.h
    zneutubeswcsignalfitter.cpp zneutubeswcsignalfitter.h
    zneutubeswcreconnect.cpp zneutubeswcreconnect.h
    zneutubeswcregionsampling.cpp zneutubeswcregionsampling.h
    zneutubetraceconnect.cpp zneutubetraceconnect.h
    zneutubetraceswclocseg.cpp zneutubetraceswclocseg.h
    zneutubetraceswclabelstack.cpp zneutubetraceswclabelstack.h
    zneutubetracep2p.cpp zneutubetracep2p.h
    zneutubevoxel.h
    zneutubevoxelarray.cpp zneutubevoxelarray.h
    zneutubelocalneuroseg.cpp zneutubelocalneuroseg.h
    zneutubelocsegchain.cpp zneutubelocsegchain.h
    zneutubelocsegchaincircle.cpp zneutubelocsegchaincircle.h
    zneutubelocsegchaininterpolate.cpp zneutubelocsegchaininterpolate.h
    zneutubelocsegchainknot.cpp zneutubelocsegchainknot.h
    zneutubelocsegchainmetrics.cpp zneutubelocsegchainmetrics.h
    zneutubelocsegchainshortestpath.cpp zneutubelocsegchainshortestpath.h
    zneutubelocsegchaintrace.cpp zneutubelocsegchaintrace.h
    zrunneutucommand2.cpp zrunneutucommand2.h
)

add_library(neutube STATIC ${NEUTUBE_SOURCES})

target_include_directories(neutube PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(neutube PUBLIC
  img
  gflags::gflags
  Qt6::Core)
