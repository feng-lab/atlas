cd /d "%~dp0"

set currDIR=%CD%
set srcDIR=%currDIR%\..\..\..\GeometricTools\WildMagic5
set buildDIR=%srcDIR%\..\wildmagic-build
set installDIR=%currDIR%\wildmagic

rd /q/s %installDIR%

call "C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" x86_amd64

MSBuild.exe %srcDIR%\LibCore\LibCore_VC120.vcxproj /property:Platform=x64 /property:Configuration=Release /maxcpucount
MSBuild.exe %srcDIR%\LibMathematics\LibMathematics_VC120.vcxproj /property:Platform=x64 /property:Configuration=Release /maxcpucount

robocopy %srcDIR%\SDK %installDIR% /e

cd %currDIR%

echo off
pause
echo The batch file is complete.