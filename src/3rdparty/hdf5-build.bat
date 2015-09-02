cd /d "%~dp0"

set currDIR=%CD%
set srcDIR=%currDIR%\..\..\..\hdf5-1.8.15-patch1
set buildDIR=%srcDIR%\..\hdf5-build
set installDIR=%currDIR%\hdf5

rd /q/s %installDIR%

rd /q/s %buildDIR%
md %buildDIR%
cd %buildDIR%

call "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" amd64
cmake -G "Visual Studio 14 2015 Win64" -DCMAKE_INSTALL_PREFIX=%installDIR% -DZLIB_LIBRARY:FILEPATH=%installDIR%/../zlib/lib/zlibstatic.lib -DHDF5_ENABLE_Z_LIB_SUPPORT:BOOL="1" -DZLIB_INCLUDE_DIR:PATH=%installDIR%/../zlib/include -DBUILD_TESTING:BOOL="0" -DHDF5_ENABLE_DEPRECATED_SYMBOLS:BOOL="0" %srcDIR%

MSBuild.exe ALL_BUILD.vcxproj /property:Configuration=Release /maxcpucount
MSBuild.exe INSTALL.vcxproj /property:Configuration=Release

cd %currDIR%
rd /q/s %buildDIR%

echo off
pause
echo The batch file is complete.