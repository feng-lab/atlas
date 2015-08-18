cd /d "%~dp0"

set currDIR=%CD%
set srcDIR=%currDIR%\..\..\..\assimp
set buildDIR=%srcDIR%\..\assimp-build
set installDIR=%currDIR%\assimp

rd /q/s %installDIR%

rd /q/s %buildDIR%
md %buildDIR%
cd %buildDIR%

call "C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" x86_amd64
cmake -G "Visual Studio 12 2013 Win64" -DASSIMP_BUILD_ASSIMP_TOOLS:BOOL="0" -DASSIMP_BUILD_TESTS:BOOL="0" -DASSIMP_ENABLE_BOOST_WORKAROUND:BOOL="1" -DASSIMP_INSTALL_PDB:BOOL="0" -DCMAKE_INSTALL_PREFIX=%installDIR% -DZLIB_LIBRARY_REL:FILEPATH=%installDIR%/../zlib/lib/zlibstatic.lib -DZLIB_INCLUDE_DIR:PATH=%installDIR%/../zlib/include %srcDIR%

MSBuild.exe ALL_BUILD.vcxproj /property:Configuration=Release /maxcpucount
MSBuild.exe INSTALL.vcxproj /property:Configuration=Release

cd %currDIR%
rd /q/s %buildDIR%

echo off
pause
echo The batch file is complete.