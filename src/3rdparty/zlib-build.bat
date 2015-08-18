cd /d "%~dp0"

set currDIR=%CD%
set srcDIR=%currDIR%\..\..\..\zlib-1.2.8
set buildDIR=%srcDIR%\..\zlib-build
set installDIR=%currDIR%\zlib

rd /q/s %installDIR%

rd /q/s %buildDIR%
md %buildDIR%
cd %buildDIR%

call "C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" x86_amd64
cmake -G "Visual Studio 12 2013 Win64" -DCMAKE_INSTALL_PREFIX=%installDIR% %srcDIR%

MSBuild.exe ALL_BUILD.vcxproj /property:Configuration=Release /maxcpucount
MSBuild.exe INSTALL.vcxproj /property:Configuration=Release

cd %currDIR%
rd /q/s %buildDIR%

echo off
pause
echo The batch file is complete.