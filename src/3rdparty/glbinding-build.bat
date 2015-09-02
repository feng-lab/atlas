cd /d "%~dp0"

set currDIR=%CD%
set srcDIR=%currDIR%\..\..\..\glbinding
set buildDIR=%srcDIR%\..\glbinding-build
set installDIR=%currDIR%\glbinding

rd /q/s %installDIR%

rd /q/s %buildDIR%
md %buildDIR%
cd %buildDIR%

call "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" amd64
cmake -G "Visual Studio 14 2015 Win64" -DCMAKE_INSTALL_PREFIX=%installDIR% -DOPTION_BUILD_STATIC=ON -DOPTION_BUILD_TESTS=OFF %srcDIR%

MSBuild.exe ALL_BUILD.vcxproj /property:Configuration=Release /maxcpucount
MSBuild.exe INSTALL.vcxproj /property:Configuration=Release

cd %currDIR%
rd /q/s %buildDIR%

echo off
pause
echo The batch file is complete.