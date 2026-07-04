# Atlas

[![macOS](https://github.com/feng-lab/atlas/actions/workflows/macOS.yml/badge.svg)](https://github.com/feng-lab/atlas/actions/workflows/macOS.yml)
[![Linux](https://github.com/feng-lab/atlas/actions/workflows/Linux.yml/badge.svg)](https://github.com/feng-lab/atlas/actions/workflows/Linux.yml)
[![Windows](https://github.com/feng-lab/atlas/actions/workflows/Windows.yml/badge.svg)](https://github.com/feng-lab/atlas/actions/workflows/Windows.yml)

## Documentation

- User Guide: [Atlas User Guide](docs/USER_GUIDE.md)
- Developer Guide: [Atlas Developer Guide](docs/DEVELOPER_GUIDE.md)
- Agents Guide: [Atlas Agents](docs/AGENTS_GUIDE.md)

## Installation
### All: Python >=3.12 (required for build scripts)
Atlas build scripts require Python >=3.12 plus a few packages from PyPI (via `pip`). Any Python installers work (e.g. conda, `uv`, pyenv, or a custom install).

For the standalone `zimg` wheel, the Stable ABI floor is fixed by packaging
configuration, not by the build interpreter. Building with newer regular
CPython is supported as long as the wheel continues to target that configured
`abi3` floor.

### macOS:
* install xcode
* install homebrew **without installing xcode command line tools** and some required packages
    ```bash
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/feng-lab/homebrew-install/master/install.sh)"
    # if xcode command line tools are already installed, disable it:
    sudo xcode-select -switch /Applications/Xcode.app`
    # some required packages
    brew install p7zip git golang autoconf
    ```
* install qt6 (by aqt or installer from the qt website)
    ```bash
    # Activate your Python environment (conda/venv/uv/etc.)
    # e.g. conda activate pt12
    # install qt by aqt
    python -m pip install --upgrade --no-cache-dir aqtinstall requests packaging build twine boto3
    cd ~
    rm -rf Qt
    mkdir Qt
    # refer to https://download.qt.io/online/qtsdkrepository/mac_x64/desktop/
    aqt install-qt --outputdir ~/Qt mac desktop 6.9.3 clang_64
    # install tools: refer to https://download.qt.io/online/qtsdkrepository/mac_x64/ifw/
    aqt install-tool --outputdir ~/Qt mac desktop tools_ifw qt.tools.ifw.47
    # list modules
    aqt list-tool mac desktop
    ```
* install Vulkan SDK 1.3+ (and ensure your GPU driver exposes Vulkan 1.3): https://vulkan.lunarg.com/home/welcome

### Windows:
* install visual studio 2022 and git
* for Atlas' clang-cl Windows path, install the official LLVM for Windows into the repository-local `llvm/` folder. Atlas probes `<repo>/llvm` first and also supports `C:\Program Files\LLVM`.
* install Perl 5.10.0 or newer and add it to `PATH`; use a Perl 5 distribution, not Perl 6. Strawberry Perl is a good default choice.
* install qt6 (by aqt or installer from the qt website)
    ```powershell
    # Activate your Python environment (conda/venv/uv/etc.)
    # e.g. conda activate pt12
    # install qt by aqt
    python -m pip install --upgrade --no-cache-dir aqtinstall requests packaging build twine boto3
    cd c:
    rm -r Qt
    mkdir Qt
    # refer to https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/
    aqt install-qt --outputdir c:/Qt windows desktop 6.9.3 win64_msvc2022_64
    # install tools: refer to https://download.qt.io/online/qtsdkrepository/windows_x86/ifw/
    aqt install-tool --outputdir c:/Qt windows desktop tools_ifw qt.tools.ifw.47
    # list modules
    aqt list-tool windows desktop
    ```
* install Vulkan SDK 1.3+ (and ensure your GPU driver exposes Vulkan 1.3): https://vulkan.lunarg.com/home/welcome
* install golang: https://golang.org, add to path

### Linux:
* install some required packages
    ```bash
    sudo apt-get update
    sudo apt-get install \
         curl wget ca-certificates gnupg lsb-release software-properties-common \
         zsh build-essential git zip unzip rsync p7zip-full \
         nasm gperf patchelf libgl1-mesa-dev libxrender-dev libxcursor-dev libglfw3-dev \
         libfreetype6-dev libfontconfig1-dev \
         libxcb-xinerama0 libxkbcommon0 libfontconfig1 libxcb-icccm4 libxcb-keysyms1 libxcb-image0 \
         libxcb-render-util0 libxcb-shape0 libxcb-xkb1 libxkbcommon-x11-0 \
         golang-go
    ```
* install LLVM/clang (Atlas currently builds with clang on Linux; GitHub Actions uses clang 22)
    ```bash
    curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh
    chmod +x /tmp/llvm.sh
    sudo /tmp/llvm.sh 22
    sudo apt-get install clang-22 lld-22 clang-tools-22
    ```
* install qt6 (by aqt or installer from the qt website)
    ```bash
    # Activate your Python environment (conda/venv/uv/etc.)
    # e.g. conda activate pt12
    # install qt by aqt
    python -m pip install --upgrade --no-cache-dir aqtinstall requests packaging build twine boto3
    cd ~
    rm -rf Qt
    mkdir Qt
    # refer to https://download.qt.io/online/qtsdkrepository/linux_x64/desktop/
    aqt install-qt --outputdir ~/Qt linux desktop 6.9.3 linux_gcc_64
    # install tools: refer to https://download.qt.io/online/qtsdkrepository/linux_x64/ifw/
    aqt install-tool --outputdir ~/Qt linux desktop tools_ifw qt.tools.ifw.47
    # list modules
    aqt list-tool linux desktop
    ```
* install Vulkan SDK 1.3+ (and ensure your GPU driver exposes Vulkan 1.3): https://vulkan.lunarg.com/home/welcome

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
    python util/build_ext_libs.py all
    # build atlas
    python util/build_and_deploy_atlas.py [--skip-test|--run-test]
    # Windows: keep Release optimization and also emit Atlas.pdb
    python util/build_and_deploy_atlas.py --release-pdb [--skip-test|--run-test]
    # Note: tests run by default, except for clean release-tag builds (vX.Y[.Z...])
    # where tests are skipped by default. Use --run-test to force tests on tags.
    # or build CMakeLists.txt
    ```

## C++ Version Defines
* $Repository_DIR/CMakeLists.txt: set(CMAKE_CXX_STANDARD 20)
* $Repository_DIR/util/build_ext_libs.py: def cpp_standard() ... // fix to 20 now
* $Repository_DIR/src/python/CMakeLists.txt: set(CMAKE_CXX_STANDARD 20)

## Minimum macOS Defines
* $Repository_DIR/CMakeLists.txt: set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0)
* $Repository_DIR/util/build_ext_libs.py: def macos_min_version() ...
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
# Builds the wheel; uploads to PyPI only when GIT_VERSION is exactly at a tag.
# The builder may be regular CPython at or above the minimum supported version;
# the published wheel target remains the configured `abi3` floor.
python util/publish_zimg.py

# install zimg from pypi
pip install zimg
```
