cd /d "%~dp0"

set currDIR=%CD%
set srcDIR=%currDIR%\..\..\..\VTK
set buildDIR=%srcDIR%\..\__vtk-build
set installDIR=%currDIR%\vtk

rd /q/s %installDIR%

rd /q/s %buildDIR%
md %buildDIR%
cd %buildDIR%

call "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" amd64
cmake -G "Visual Studio 14 2015 Win64" -DCMAKE_INSTALL_PREFIX=%installDIR% -DZLIB_LIBRARY:FILEPATH="%currDIR%/zlib/lib/zlibstatic.lib" -DZLIB_INCLUDE_DIR:PATH="%currDIR%/zlib/include" -DBUILD_TESTING:BOOL="0" -DVTK_USE_SYSTEM_ZLIB:BOOL="1" -DVTK_LEGACY_REMOVE:BOOL="1" %srcDIR%

MSBuild.exe ALL_BUILD.vcxproj /property:Configuration=Release /maxcpucount
MSBuild.exe INSTALL.vcxproj /property:Configuration=Release

cd %currDIR%
rd /q/s %buildDIR%

echo off
pause
echo The batch file is complete.