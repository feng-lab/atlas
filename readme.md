## Installation
### macOS:
1. install xcode, homebrew, qt5, intel c composer (for mkl, ipp, tbb)
2. brew install unrar p7zip git cmake
3. current code repository folder should be named "atlas" and make sure "atlas_others" folder and "atlas" folder are in same directory
4. (setup SSH credential for github), run "build" from 3rdparty folder
5. build atlas/src/CMakeLists.txt

### Windows:
1. patch libtiff win32 TIFFFdOpen
2. run "build" from 3rdparty folder
3. build atlas/src/CMakeLists.txt
