## Installation
### macOS:
* install xcode, homebrew, qt5 (>= 5.9), intel c composer (for mkl, ipp, tbb) and python3
* `brew install unrar p7zip git golang`
* disable command line tools: `sudo xcode-select -switch /Applications/Xcode.app`

### Windows:
* install visual studio, qt5 (>= 5.9), intel c composer (for mkl, ipp, tbb), python3, git
* install vulkan sdk: https://vulkan.lunarg.com/home/welcome
* install golang: https://golang.org

### Linux:
* install qt5 (>= 5.9), mkl, ipp, tbb, python3 and ninja
* `sudo apt install unrar p7zip-full git nasm libglfw3-dev zlib1g-dev libssl-dev golang patchelf`
* install vulkan sdk: https://vulkan.lunarg.com/home/welcome with apt

### All:
* `atlas_others` folder and repository folder should be in same directory
* (setup SSH credential for github), run `python3 util/build_ext_libs.py all` to build external libraries
* run `python3 util/build_and_deploy_atlas.py all` or build CMakeLists.txt

## C++ Version Defines
* $Repository_DIR/CMakeLists.txt: set(CMAKE_CXX_STANDARD 17)
* $Repository_DIR/util/build_ext_libs.py: def get_cmake_cmd_common_part(install_dir: str) ...
* $Repository_DIR/src/3rdparty/freeimage-makefiles/Makefile_fip: -stdlib=libc++ -std=c++14 // todo: change to 17
* $Repository_DIR/src/3rdparty/freeimage-makefiles/Makefile_gun: -stdlib=libc++ -std=c++14 // todo: change to 17
* $Repository_DIR/src/3rdparty/makeengine.macos.gte: -std=c++14 -stdlib=libc++

## Minimum macOS Defines
* $Repository_DIR/CMakeLists.txt: set(CMAKE_OSX_DEPLOYMENT_TARGET 10.12)
* $Repository_DIR/util/build_ext_libs.py: def macos_min_version() ...
* $Repository_DIR/src/3rdparty/freeimage-makefiles/Makefile_fip: -mmacosx-version-min=10.12
* $Repository_DIR/src/3rdparty/freeimage-makefiles/Makefile_gun: -mmacosx-version-min=10.12
* $Repository_DIR/src/3rdparty/makeengine.macos.gte: -mmacosx-version-min=10.12

## Visual Studio Update
* $Repository_DIR/util/common_dirs.py: 178: def vs_install_dir() -> str ...
* $Repository_DIR/util/build_atlas.py: 11: '-G', 'Visual Studio 16 2019', '-A', 'x64', '-T', 'host=x64'
* $Repository_DIR/util/build_ext_libs.py: 133: '-G', 'Visual Studio 16 2019', '-A', 'x64', '-T', 'host=x64'
* $Repository_DIR/util/build_ext_libs.py: change all 'v141' to 'v142'
* $Repository_DIR/util/build_ext_libs.py: change one 'vc15' to 'vc16'

## Python Package Build
```bash
conda install conda-build cmake ninja qt mkl-devel tbb-devel numpy pybind11
conda build purge-all
conda build conda
conda remove zimg -y
conda install zimg -c fenglab -y
```
