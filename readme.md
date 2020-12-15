## Installation
### macOS:
* install xcode
* install homebrew **without installing xcode command line tools**
    ```bash
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/feng-lab/homebrew-install/master/install.sh)"
    # if xcode command line tools are already installed, disable it:
    sudo xcode-select -switch /Applications/Xcode.app`
    ```
* install qt (>= 6.0), intel oneapi basekit and python3 (recommend miniconda)
* `brew install unrar p7zip git golang autoconf`

### Windows:
* install visual studio, qt (>= 6.0), intel oneapi basekit, python3, git
* install vulkan sdk: https://vulkan.lunarg.com/home/welcome
* install golang: https://golang.org, add to path
* install perl: https://www.activestate.com/products/perl/downloads/, add to path

### Linux:
* install qt (>= 6.0), python3
* `sudo apt install unrar p7zip-full git nasm golang patchelf libxcursor-dev`
* install vulkan sdk: https://vulkan.lunarg.com/home/welcome with apt
* install intel oneapi basekit with apt

### All:
* `atlas_others` folder and repository folder should be in same directory
* get submodules
    ```bash
    git submodule update --init --recursive --depth 1
    # or git submodule update --init --recursive
    ```
* run `python3 util/build_ext_libs.py all` to build external libraries
* run `python3 util/build_and_deploy_atlas.py` or build CMakeLists.txt

## C++ Version Defines
* $Repository_DIR/CMakeLists.txt: set(CMAKE_CXX_STANDARD 17)
* $Repository_DIR/util/build_ext_libs.py: def get_cmake_cmd_common_part(install_dir: str) ...
* $Repository_DIR/src/3rdparty/freeimage-makefiles/Makefile_fip: -stdlib=libc++ -std=c++14 // todo: change to 17
* $Repository_DIR/src/3rdparty/freeimage-makefiles/Makefile_gun: -stdlib=libc++ -std=c++14 // todo: change to 17
* $Repository_DIR/src/3rdparty/makeengine.macos.gte: -std=c++14 -stdlib=libc++
* $Repository_DIR/src/python/CMakeLists.txt: set(CMAKE_CXX_STANDARD 17)

## Minimum macOS Defines
* $Repository_DIR/CMakeLists.txt: set(CMAKE_OSX_DEPLOYMENT_TARGET 10.14)
* $Repository_DIR/util/build_ext_libs.py: def macos_min_version() ...
* $Repository_DIR/src/3rdparty/freeimage-makefiles/Makefile_fip: -mmacosx-version-min=10.14
* $Repository_DIR/src/3rdparty/freeimage-makefiles/Makefile_gun: -mmacosx-version-min=10.14
* $Repository_DIR/src/python/CMakeLists.txt: set(CMAKE_OSX_DEPLOYMENT_TARGET 10.14)
* $SuiteSparse_Repo/CMakeLists.txt: set(CMAKE_OSX_DEPLOYMENT_TARGET 10.14)

## Visual Studio Update
* $Repository_DIR/util/common_dirs.py: 178: def vs_install_dir() -> str ...
* $Repository_DIR/util/build_atlas.py: 11: '-G', 'Visual Studio 16 2019', '-A', 'x64', '-T', 'host=x64'
* $Repository_DIR/util/build_ext_libs.py: 133: '-G', 'Visual Studio 16 2019', '-A', 'x64', '-T', 'host=x64'
* $Repository_DIR/util/build_ext_libs.py: change all 'v141' to 'v142'
* $Repository_DIR/util/build_ext_libs.py: change one 'vc15' to 'vc16'

## Python Package Build
```bash
conda install conda-build cmake ninja qt numpy
conda build purge-all
conda build conda
conda remove zimg -y
conda install zimg -c fenglab -y
```
