cd /d "%~dp0"

set currDIR=%CD%
set srcDIR=%currDIR%\..\..\..\botan
set buildDIR=%srcDIR%\..\botan-build
set installDIR=%currDIR%\botan

rd /q/s %installDIR%

rd /q/s %buildDIR%
md %buildDIR%
#cd %buildDIR%
cd %srcDIR%

call "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" amd64
C:\Python27\python.exe configure.py --cc=msvc --prefix=%installDIR%

nmake
botan.exe test
nmake install

rd /q/s build
del /f /q Makefile
del /f /q *.pdb

cd %currDIR%
rd /q/s %buildDIR%

echo off
pause
echo The batch file is complete.