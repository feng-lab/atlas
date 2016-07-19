cd /d "%~dp0"

set currDIR=%CD%
set srcDIR=%currDIR%\..\..\..\libpng-1.6.23
set buildDIR=%srcDIR%\..\__libpng-build
set installDIR=%currDIR%\libpng

rd /q/s %installDIR%

rd /q/s %buildDIR%
md %buildDIR%
cd %buildDIR%

call "C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" x86_amd64
cmake -G "Visual Studio 12 2013 Win64" -DCMAKE_INSTALL_PREFIX=%installDIR% -DZLIB_LIBRARY:FILEPATH=%installDIR%/../zlib/lib/zlibstatic.lib -DZLIB_INCLUDE_DIR:PATH=%installDIR%/../zlib/include -DPNG_TESTS:BOOL="0" -DPNG_SHARED:BOOL="0" %srcDIR%

MSBuild.exe ALL_BUILD.vcxproj /property:Configuration=Release /maxcpucount
MSBuild.exe INSTALL.vcxproj /property:Configuration=Release

cd %currDIR%
rd /q/s %buildDIR%

echo off
pause
echo The batch file is complete.