cmake_minimum_required(VERSION 3.6.1)
project(atlas)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../build-atlas)

add_subdirectory(img)
add_subdirectory(atlas)

