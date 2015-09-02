cd /d "%~dp0"

set currDIR=%CD%
set srcDIR=%currDIR%\..\..\..\FreeImage
set buildDIR=%srcDIR%\..\__freeimage-build
set installDIR=%currDIR%\freeimage

rd /q/s %installDIR%

cd %srcDIR%

call "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" amd64

MSBuild.exe FreeImage.2013.sln /target:FreeImagePlus /property:Platform=x64 /property:Configuration=Release /maxcpucount

robocopy %srcDIR%\Dist\x64 %installDIR% /e
robocopy %srcDIR%\Wrapper\FreeImagePlus\dist\x64 %installDIR% /e

cd %currDIR%

echo off
pause
echo The batch file is complete.