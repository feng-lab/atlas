# Atlas

[![macOS](https://github.com/feng-lab/atlas/workflows/macOS/badge.svg)](https://github.com/feng-lab/atlas/actions?query=workflow:macOS)
[![Linux](https://github.com/feng-lab/atlas/workflows/Linux/badge.svg)](https://github.com/feng-lab/atlas/actions?query=workflow:Linux)
[![Windows](https://github.com/feng-lab/atlas/workflows/Windows/badge.svg)](https://github.com/feng-lab/atlas/actions?query=workflow:Windows)

## Documentation

- User Guide: [Atlas User Guide](docs/USER_GUIDE.md)
- Developer Guide: [Atlas Developer Guide](docs/DEVELOPER_GUIDE.md)
- Agents Guide: [Atlas Agents (Unified)](docs/AGENTS_GUIDE.md)

## Installation
### All: install python3 for build scripts and for building the zimg conda package
```bash
# install miniconda
# then
conda env remove -n pt12 -y
conda create -n pt12 -y python=3.12
conda activate pt12
conda install numpy python conda-build anaconda-client pip
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
    conda activate pt12
    # install qt by aqt
    pip install --upgrade --no-cache-dir aqtinstall
    cd ~
    rm -rf Qt
    mkdir Qt
    # refer to https://download.qt.io/online/qtsdkrepository/mac_x64/desktop/
    aqt install-qt --outputdir ~/Qt mac desktop 6.9.2 clang_64 # --external 7z
    # install tools: refer to https://download.qt.io/online/qtsdkrepository/mac_x64/desktop/tools_ifw/
    aqt install-tool --outputdir ~/Qt mac desktop tools_ifw qt.tools.ifw.47
    # list modules
    aqt list-tool mac desktop
    ```
* install intel oneapi basekit
* fix intel oneapi basekit ipp cmake error: add 'set(IPP_ARCH)' to file `oneapi/ipp/latest/lib/cmake/ipp/ipp-config.cmake`

### Windows:
* install visual studio 2022, intel oneapi basekit, git
* install qt6 (by aqt or installer from the qt website)
    ```powershell
    conda activate pt12
    # install qt by aqt
    pip install --upgrade --no-cache-dir aqtinstall
    cd c:
    rm -r Qt
    mkdir Qt
    # refer to https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/
    aqt install-qt --outputdir c:/Qt windows desktop 6.9.2 win64_msvc2022_64
    aqt install-qt --outputdir c:/Qt windows desktop 6.9.2 win64_mingw
    # install tools: refer to https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/tools_ifw/
    aqt install-tool --outputdir c:/Qt windows desktop tools_ifw qt.tools.ifw.47
    # list modules
    aqt list-tool windows desktop
    ```
* install Vulkan SDK 1.3+ (and ensure your GPU driver exposes Vulkan 1.3): https://vulkan.lunarg.com/home/welcome
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
    conda activate pt12
    # install qt by aqt
    pip install --upgrade --no-cache-dir aqtinstall
    cd ~
    rm -rf Qt
    mkdir Qt
    # refer to https://download.qt.io/online/qtsdkrepository/linux_x64/desktop/
    aqt install-qt --outputdir ~/Qt linux desktop 6.9.2 linux_gcc_64 # --external 7z
    # install tools: refer to https://download.qt.io/online/qtsdkrepository/linux_x64/desktop/tools_ifw/
    aqt install-tool --outputdir ~/Qt linux desktop tools_ifw qt.tools.ifw.47
    # list modules
    aqt list-tool linux desktop
    ```
* install Vulkan SDK 1.3+ (and ensure your GPU driver exposes Vulkan 1.3). On Debian/Ubuntu, prefer LunarG packages or your distro’s Vulkan 1.3 packages: https://vulkan.lunarg.com/home/welcome
* install intel oneapi basekit with apt

### All:
* clone atlas repo
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
    python3 util/build_and_deploy_atlas.py [--skip-test]
    # or build CMakeLists.txt
    ```

### macOS signing & notarization (deployment)
`util/deploy_atlas.py` codesigns nested code inside-out (without `codesign --deep`) and notarizes by default. It also generates a signed/notarized QtIFW `MaintenanceTool.app` (via `binarycreator --mt`) and ships it in the `fenglab.maintenance` package so the installed Maintenance Tool is signed. Configure via env vars:
- `MACOS_CODESIGN_IDENTITY` (required when signing is enabled)
- `MACOS_NOTARYTOOL_API_KEY_PATH`, `MACOS_NOTARYTOOL_API_KEY_ID`, `MACOS_NOTARYTOOL_API_ISSUER_ID` (required for notarization)
- `ATLAS_MACOS_CODESIGN_ENTITLEMENTS` (optional entitlements plist)
- `ATLAS_MACOS_DISABLE_SIGNING=1` to use ad-hoc signing and skip notarization

## C++ Version Defines
* $Repository_DIR/CMakeLists.txt: set(CMAKE_CXX_STANDARD 20)
* $Repository_DIR/util/build_ext_libs.py: def cpp_standard() ... // fix to 20 now
* $Repository_DIR/src/3rdparty/freeimage-makefiles/Makefile_fip: -stdlib=libc++ -std=c++14 // todo: change to 20
* $Repository_DIR/src/3rdparty/freeimage-makefiles/Makefile_gun: -stdlib=libc++ -std=c++14 // todo: change to 20
* $Repository_DIR/src/python/CMakeLists.txt: set(CMAKE_CXX_STANDARD 20)

## Minimum macOS Defines
* $Repository_DIR/CMakeLists.txt: set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0)
* $Repository_DIR/util/build_ext_libs.py: def macos_min_version() ...
* $Repository_DIR/src/3rdparty/freeimage-makefiles/Makefile_fip: change all -mmacosx-version-min=12.0
* $Repository_DIR/src/3rdparty/freeimage-makefiles/Makefile_gun: change all -mmacosx-version-min=12.0
* $Repository_DIR/src/python/CMakeLists.txt: set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0)

## Visual Studio Update
* $Repository_DIR/util/common_dirs.py: 200: def vs_install_dir() -> str ...
* $Repository_DIR/util/common_dirs.py: change all 'v142' to 'v143'
* $Repository_DIR/util/build_atlas.py: 19: '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'host=x64'
* $Repository_DIR/util/build_ext_libs.py: 225: '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'host=x64'
* $Repository_DIR/util/build_ext_libs.py: change all 'v142' to 'v143'
* $Repository_DIR/util/build_ext_libs.py: change one 'vc16' to 'vc17'

## Python Package Build
```bash
python util/publish_zimg.py

# install zimg from pypi
pip install zimg
# install zimg from conda
conda install zimg -c fenglab -y
```
