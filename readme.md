## Installation
### macOS:
* install xcode, homebrew, qt5, intel c composer (for mkl, ipp, tbb) and python3
* brew install unrar p7zip git cmake ninja

### Windows:
* install visual studio, qt5, intel c composer (for mkl, ipp, tbb), python3, git

### Linux:
* install qt5, mkl, ipp, tbb, python3 and ninja
* sudo apt install unrar p7zip-full git nasm libglfw3-dev zlib1g-dev patchelf

### All:
* code repository folder should be named as "atlas"; "atlas_others" folder and "atlas" folder should be in same directory
* (setup SSH credential for github), run "python3 util/build_ext_libs.py all" to build external libraries
* run "python3 util/build_and_deploy_atlas.py all" or build CMakeLists.txt
