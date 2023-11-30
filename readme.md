# Atlas

[![macOS](https://github.com/feng-lab/atlas/workflows/macOS/badge.svg)](https://github.com/feng-lab/atlas/actions?query=workflow:macOS)
[![Linux](https://github.com/feng-lab/atlas/workflows/Linux/badge.svg)](https://github.com/feng-lab/atlas/actions?query=workflow:Linux)
[![Windows](https://github.com/feng-lab/atlas/workflows/Windows/badge.svg)](https://github.com/feng-lab/atlas/actions?query=workflow:Windows)

## Installation
### All: install python3 for build scripts and for building the zimg conda package
```bash
# install miniconda
# then
conda env remove -n pt11 -y
conda create -n pt11 -y python=3.11
conda activate pt11
conda install tbb-devel mkl-devel qt numpy python mkl numpy tbb conda-build conda-verify ninja
```

### macOS:
* install xcode
* install homebrew **without installing xcode command line tools** and some required packages
    ```bash
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/feng-lab/homebrew-install/master/install.sh)"
    # if xcode command line tools are already installed, disable it:
    sudo xcode-select -switch /Applications/Xcode.app`
    # some required packages
    brew install unrar p7zip git golang autoconf
    ```
* install qt6 (by aqt or installer from the qt website)
    ```bash
    conda activate pt11
    # install qt by aqt
    pip install --upgrade --no-cache-dir aqtinstall
    cd ~
    rm -rf Qt
    mkdir Qt
    # refer to https://download.qt.io/online/qtsdkrepository/mac_x64/desktop/
    aqt install-qt --outputdir ~/Qt mac desktop 6.6.0 clang_64 --external 7z
    # install tools: refer to https://download.qt.io/online/qtsdkrepository/mac_x64/desktop/tools_ifw/
    aqt install-tool --outputdir ~/Qt mac desktop tools_ifw qt.tools.ifw.46
    # list modules
    aqt list-tool mac desktop
    ```
* install intel oneapi basekit
* fix intel oneapi basekit ipp cmake error: add 'set(IPP_ARCH)' to file `oneapi/ipp/latest/lib/cmake/ipp/ipp-config.cmake`

### Windows:
* install visual studio 2022, intel oneapi basekit, git
* install qt6 (by aqt or installer from the qt website)
    ```bash
    conda activate pt11
    # install qt by aqt
    pip install --upgrade --no-cache-dir aqtinstall
    cd ~
    rm -rf Qt
    mkdir Qt
    # refer to https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/
    aqt install-qt --outputdir ~/Qt windows desktop 6.6.0 win64_msvc2019_64 --external 7z
    aqt install-qt --outputdir ~/Qt windows desktop 6.6.0 win64_mingw --external 7z
    # install tools: refer to https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/tools_ifw/
    aqt install-tool --outputdir ~/Qt windows desktop tools_ifw qt.tools.ifw.46
    # list modules
    aqt list-tool windows desktop
    ```
* install vulkan sdk: https://vulkan.lunarg.com/home/welcome
* install golang: https://golang.org, add to path
* install perl: https://strawberryperl.com/, add to path

### Linux:
* install some required packages
    ```bash
    sudo apt install zip unrar p7zip-full git nasm golang patchelf libxcursor-dev build-essential libglfw3-dev \
         libxcb-xinerama0 libxkbcommon0 libfontconfig1 libxcb-icccm4 libxcb-keysyms1 libxcb-image0 \
         libxcb-render-util0 libxcb-shape0 libxcb-xkb1 libxkbcommon-x11-0
    ```
* install qt6 (by aqt or installer from the qt website)
    ```bash
    conda activate pt11
    # install qt by aqt
    pip install --upgrade --no-cache-dir aqtinstall
    cd ~
    rm -rf Qt
    mkdir Qt
    # refer to https://download.qt.io/online/qtsdkrepository/linux_x64/desktop/
    aqt install-qt --outputdir ~/Qt linux desktop 6.6.0 gcc_64 --external 7z
    # install tools: refer to https://download.qt.io/online/qtsdkrepository/linux_x64/desktop/tools_ifw/
    aqt install-tool --outputdir ~/Qt linux desktop tools_ifw qt.tools.ifw.46
    # list modules
    aqt list-tool linux desktop
    ```
* install vulkan sdk: https://vulkan.lunarg.com/home/welcome with apt
* install intel oneapi basekit with apt

### All:
* clone atlas repo
* `atlas_deps` folder should be in home directory. 
  * `atlas_deps` folder can be downloaded from https://www.dropbox.com/scl/fo/uw1tj7n1tl6gswb2nb8ma/h?rlkey=dbe2tj3a4frld6ifsv1cmqobr&dl=0
* (not required for building): `atlas_test_data` folder should be in home directory.
  * `atlas_test_data` folder can be downloaded from https://www.dropbox.com/scl/fo/n816eimuynywew9pyfec3/h?rlkey=of5b1hmany7yopder5hj41y8x&dl=0
* get submodules
    ```bash
    # in atlas repo
    git submodule update --init --recursive
    ```
* build atlas
    ```bash
    # in atlas repo
    # build external libraries and python packages
    python3 util/build_ext_libs.py all
    # build atlas
    python3 util/build_and_deploy_atlas.py
    # or build CMakeLists.txt
    # see `utils/all.sh` for example
    ```

## C++ Version Defines
* $Repository_DIR/CMakeLists.txt: set(CMAKE_CXX_STANDARD 17)
* $Repository_DIR/util/build_ext_libs.py: def cpp_standard() ... // fix to 17 now
* $Repository_DIR/src/3rdparty/freeimage-makefiles/Makefile_fip: -stdlib=libc++ -std=c++14 // todo: change to 17
* $Repository_DIR/src/3rdparty/freeimage-makefiles/Makefile_gun: -stdlib=libc++ -std=c++14 // todo: change to 17
* $Repository_DIR/src/python/CMakeLists.txt: set(CMAKE_CXX_STANDARD 17)

## Minimum macOS Defines
* $Repository_DIR/CMakeLists.txt: set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0)
* $Repository_DIR/util/build_ext_libs.py: def macos_min_version() ...
* $Repository_DIR/src/3rdparty/freeimage-makefiles/Makefile_fip: change all -mmacosx-version-min=11.0
* $Repository_DIR/src/3rdparty/freeimage-makefiles/Makefile_gun: change all -mmacosx-version-min=11.0
* $Repository_DIR/src/python/CMakeLists.txt: set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0)
* $SuiteSparse_Repo/CMakeLists.txt: set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0)

## Visual Studio Update
* $Repository_DIR/util/common_dirs.py: 200: def vs_install_dir() -> str ...
* $Repository_DIR/util/common_dirs.py: change all 'v142' to 'v143'
* $Repository_DIR/util/build_atlas.py: 19: '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'host=x64'
* $Repository_DIR/util/build_ext_libs.py: 225: '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'host=x64'
* $Repository_DIR/util/build_ext_libs.py: change all 'v142' to 'v143'
* $Repository_DIR/util/build_ext_libs.py: change one 'vc16' to 'vc17'

## Python Package Build
```bash
conda-build purge-all
conda-build zimg-recipe
conda remove zimg -y
conda install zimg -c fenglab -y
```
