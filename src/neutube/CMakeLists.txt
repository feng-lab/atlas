cmake_minimum_required(VERSION 3.17.5)
project(neutube)

find_package(Qt6 ${QT_VERSION} REQUIRED COMPONENTS Core
             PATHS ${QT_HOST_PATH} NO_DEFAULT_PATH)
print_target_properties(Qt6::Core)

set(NEUTUBE_SOURCES
    zimgstackinterface.cpp zimgstackinterface.h
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
