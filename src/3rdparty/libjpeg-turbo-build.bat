cd /d "%~dp0"

set currDIR=%CD%
set srcDIR=%currDIR%\..\..\..\libjpeg-turbo-1.4.2
set buildDIR=%srcDIR%\..\__libjpeg-turbo-build
set installDIR=%currDIR%\libjpeg-turbo

rd /q/s %installDIR%

rd /q/s %buildDIR%
md %buildDIR%
cd %buildDIR%

call "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" amd64
cmake -G "Visual Studio 14 2015 Win64" -DCMAKE_INSTALL_PREFIX=%installDIR% -DENABLE_SHARED:BOOL="0" -DNASM:PATH=%currDIR%\nasm.exe %srcDIR%

MSBuild.exe ALL_BUILD.vcxproj /property:Configuration=Release /property:ForceImportBeforeCppTargets=%currDIR%\runtime_md.props /maxcpucount
MSBuild.exe INSTALL.vcxproj /property:Configuration=Release /property:ForceImportBeforeCppTargets=%currDIR%\runtime_md.props

cd %currDIR%
rd /q/s %buildDIR%

echo off
pause
echo The batch file is complete.