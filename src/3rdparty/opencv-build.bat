cd /d "%~dp0"

set currDIR=%CD%
set srcDIR=%currDIR%\..\..\..\opencv
set srcContribDIR=$currDIR\..\..\..\opencv_contrib
set buildDIR=%srcDIR%\..\__opencv-build
set installDIR=%currDIR%\opencv

rd /q/s %installDIR%

rd /q/s %buildDIR%
md %buildDIR%
cd %buildDIR%

call "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" amd64
cmake -G "Visual Studio 14 2015 Win64" -DCMAKE_INSTALL_PREFIX=%installDIR% -DZLIB_LIBRARY:FILEPATH="%currDIR%/zlib/lib/zlibstatic.lib" -DZLIB_INCLUDE_DIR:PATH="%currDIR%/zlib/include" -DBUILD_PNG:BOOL="0" -DBUILD_opencv_apps:BOOL="0" -DBUILD_opencv_text:BOOL="0" -DBUILD_ZLIB:BOOL="0" -DBUILD_TESTS:BOOL="0" -DWITH_FFMPEG:BOOL="0" -DWITH_WIN32UI:BOOL="0" -DWITH_WEBP:BOOL="0" -DBUILD_PACKAGE:BOOL="0" -DWITH_JASPER:BOOL="0" -DWITH_OPENEXR:BOOL="0" -DWITH_GIGEAPI:BOOL="0" -DWITH_JPEG:BOOL="0" -DOPENCV_EXTRA_MODULES_PATH:PATH="%srcContribDIR%/modules" -DWITH_IPP:BOOL="0" -DBUILD_WITH_DEBUG_INFO:BOOL="0" -DBUILD_JPEG:BOOL="0" -DBUILD_TIFF:BOOL="0" -DTBB_INCLUDE_DIRS:PATH="C:\Program Files (x86)\IntelSWTools\compilers_and_libraries\windows\tbb\include" -DTBB_STDDEF_PATH:FILEPATH="C:/Program Files (x86)/IntelSWTools/compilers_and_libraries/windows/tbb/include/tbb/tbb_stddef.h" -DTBB_LIB_DIR:PATH="C:/Program Files (x86)/IntelSWTools/compilers_and_libraries/windows/tbb/lib/intel64/vc14" -DBUILD_WITH_STATIC_CRT:BOOL="0" -DEIGEN_INCLUDE_PATH:PATH="%currDIR%/eigen" -DWITH_VTK:BOOL="0" -DBUILD_PERF_TESTS:BOOL="0" -DBUILD_JASPER:BOOL="0" -DBUILD_DOCS:BOOL="0" -DWITH_TIFF:BOOL="0" -DWITH_1394:BOOL="0" -DBUILD_OPENEXR:BOOL="0" -DWITH_DSHOW:BOOL="1" -DBUILD_SHARED_LIBS:BOOL="0" -DWITH_PNG:BOOL="0" -DWITH_TBB:BOOL="1" -DWITH_PVAPI:BOOL="0" %srcDIR%

MSBuild.exe ALL_BUILD.vcxproj /property:Configuration=Release /maxcpucount
MSBuild.exe INSTALL.vcxproj /property:Configuration=Release

cd %currDIR%
rd /q/s %buildDIR%

echo off
pause
echo The batch file is complete.