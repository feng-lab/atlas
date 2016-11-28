## Installation
### macOS:
1. install xcode, homebrew, qt5, intel c composer (for mkl, ipp, tbb)
2. brew install unrar p7zip git cmake
3. current code repository folder should be named "atlas" and make sure "atlas_others" folder and "atlas" folder are in same directory
4. (setup SSH credential for github), run "build.py all" from util folder
5. build CMakeLists.txt

### Windows:
1. patch libtiff win32 TIFFFdOpen
2. run "build.py all" from util folder
3. build CMakeLists.txt
