## Installation
### macOS:
* install xcode, homebrew, qt5 (>= 5.9), intel c composer (for mkl, ipp, tbb) and python3
* brew install unrar p7zip git golang
* disable command line tools: sudo xcode-select -switch /Applications/Xcode.app

### Windows:
* install visual studio, qt5 (>= 5.9), intel c composer (for mkl, ipp, tbb), python3, git

### Linux:
* install qt5 (>= 5.9), mkl, ipp, tbb, python3 and ninja
* sudo apt install unrar p7zip-full git nasm libglfw3-dev zlib1g-dev libssl-dev golang patchelf

### All:
* code repository folder should be named as "atlas"; "atlas_others" folder and "atlas" folder should be in same directory
* (setup SSH credential for github), run "python3 util/build_ext_libs.py all" to build external libraries
* run "python3 util/build_and_deploy_atlas.py all" or build CMakeLists.txt

## C++ Version Defines
* $ROOT_DIR/CMakeLists.txt: set(CMAKE_CXX_STANDARD 14)
* $ROOT_DIR/util/build_ext_libs.py: def get_cmake_cmd_common_part(install_dir: str) ...
* $ROOT_DIR/src/3rdparty/freeimage-makefiles/Makefile_fip: -stdlib=libc++ -std=c++14
* $ROOT_DIR/src/3rdparty/freeimage-makefiles/Makefile_gun: -stdlib=libc++ -std=c++14
* $ROOT_DIR/src/3rdparty/makeengine.macos.gte: -std=c++14 -stdlib=libc++

## Minimum macOS Defines
* $ROOT_DIR/CMakeLists.txt: set(CMAKE_OSX_DEPLOYMENT_TARGET 10.10)
* $ROOT_DIR/util/build_ext_libs.py: def macos_min_version() ...
* $ROOT_DIR/src/3rdparty/freeimage-makefiles/Makefile_fip: -mmacosx-version-min=10.10
* $ROOT_DIR/src/3rdparty/freeimage-makefiles/Makefile_gun: -mmacosx-version-min=10.10
* $ROOT_DIR/src/3rdparty/makeengine.macos.gte: -mmacosx-version-min=10.10

## Python Package Build
* conda install conda-build cmake ninja qt mkl-devel tbb-devel numpy pybind11
* conda build purge-all
* conda build conda
* conda remove zimg -y
* conda install zimg --use-local -y
