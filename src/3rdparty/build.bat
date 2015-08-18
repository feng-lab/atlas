cd /d "%~dp0"
set currDIR=%CD%

call "%currDIR%\zlib-build.bat" < nul
call "%currDIR%\glbinding-build.bat" < nul
call "%currDIR%\libjpeg-turbo-build.bat" < nul
call "%currDIR%\jxrlib-build.bat" < nul
call "%currDIR%\wildmagic-build.bat" < nul
call "%currDIR%\assimp-build.bat" < nul
call "%currDIR%\hdf5-build.bat" < nul
call "%currDIR%\freeimage-build.bat" < nul
call "%currDIR%\itk-build.bat" < nul

echo off
pause
echo The batch file is complete.