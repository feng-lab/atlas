cd /d "%~dp0"

set currDIR=%CD%
set srcDIR=%currDIR%\..\..\..\ITK
set buildDIR=%srcDIR%\..\__itk-build
set installDIR=%currDIR%\itk

rd /q/s %installDIR%

rd /q/s %buildDIR%
md %buildDIR%
cd %buildDIR%

call "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" amd64
cmake -G "Visual Studio 14 2015 Win64" -DCMAKE_INSTALL_PREFIX=%installDIR% -DBUILD_EXAMPLES:BOOL="0" -DBUILD_TESTING:BOOL="0" -DITK_USE_64BITS_IDS:BOOL="1" -DITK_LEGACY_REMOVE:BOOL="1" -DModule_ITKReview:BOOL="1" -DITK_USE_SYSTEM_ZLIB:BOOL="1" -DZLIB_LIBRARY:FILEPATH=%currDIR%/zlib/lib/zlibstatic.lib -DZLIB_INCLUDE_DIR:PATH=%currDIR%/zlib/include %srcDIR%

MSBuild.exe ALL_BUILD.vcxproj /property:Configuration=Release /maxcpucount
MSBuild.exe INSTALL.vcxproj /property:Configuration=Release

cd %currDIR%
rd /q/s %buildDIR%

echo off
pause
echo The batch file is complete.