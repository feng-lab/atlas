cmake_minimum_required(VERSION 3.17.5)
project(neutube)

find_package(Qt6 ${QT_VERSION} REQUIRED COMPONENTS Core
             PATHS ${QT_HOST_PATH} NO_DEFAULT_PATH)
print_target_properties(Qt6::Core)

set(NEUTUBE_SOURCES
    zimgstackinterface.cpp zimgstackinterface.h
    zneutubedarrayqsort.cpp zneutubedarrayqsort.h
    zneutubecompareswc.cpp zneutubecompareswc.h
    zneutubeimgbwmorph.cpp zneutubeimgbwmorph.h
    zneutubeneighborhood.cpp zneutubeneighborhood.h
    zneutubeobjlabel.cpp zneutubeobjlabel.h
    zneutubeplanaredt.cpp zneutubeplanaredt.h
    zneutubespgrow.cpp zneutubespgrow.h
    zneutubespgrowparser.cpp zneutubespgrowparser.h
    zneutubeskeletonize.cpp zneutubeskeletonize.h
    zneutubeskeletonizer.cpp zneutubeskeletonizer.h
    zneutubeswcgeom.cpp zneutubeswcgeom.h
    zneutubeswcnodeops.h
    zneutubeswcops.cpp zneutubeswcops.h
    zneutubeswcpointdist.cpp zneutubeswcpointdist.h
    zneutubeswcreconnect.cpp zneutubeswcreconnect.h
    zneutubeswcregionsampling.cpp zneutubeswcregionsampling.h
    zneutubeswcresampler.cpp zneutubeswcresampler.h
    zneutubeswcwriter.cpp zneutubeswcwriter.h
    zneutubevoxel.h
    zneutubevoxelarray.cpp zneutubevoxelarray.h
    zneutubelegacy.cpp zneutubelegacy.h
    zrunneutucommand2.cpp zrunneutucommand2.h
)

add_library(neutube STATIC ${NEUTUBE_SOURCES})

target_include_directories(neutube PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(neutube PUBLIC
  neutu
  img
  gflags::gflags
  Qt6::Core)
