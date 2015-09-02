cd /d "%~dp0"

set currDIR=%CD%
set srcDIR=%currDIR%\..\..\..\jxrlib
set buildDIR=%srcDIR%\..\__jxrlib-build
set installDIR=%currDIR%\jxrlib

rd /q/s %installDIR%

cd %srcDIR%\jxrencoderdecoder

call "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" amd64

MSBuild.exe JXR_vc14.sln /target:JXRDecApp /property:Platform=x64 /property:Configuration=Release /property:ForceImportBeforeCppTargets=%currDIR%\runtime_md.props /maxcpucount

robocopy %srcDIR%\common\include %installDIR%\include\libjxr\common *.h
robocopy %srcDIR%\image\x86 %installDIR%\include\libjxr\image\x86 *.h
robocopy %srcDIR%\image\sys %installDIR%\include\libjxr\image *.h
robocopy %srcDIR%\image\encode %installDIR%\include\libjxr\image *.h
robocopy %srcDIR%\image\decode %installDIR%\include\libjxr\image *.h
robocopy %srcDIR%\jxrgluelib %installDIR%\include\libjxr\glue *.h
robocopy %srcDIR%\jxrgluelib\Release\JXRGlueLib\x64 %installDIR%\lib *.lib
robocopy %srcDIR%\image\vc14projects\Release\JXRCommonLib\x64 %installDIR%\lib *.lib
robocopy %srcDIR%\image\vc14projects\Release\JXRDecodeLib\x64 %installDIR%\lib *.lib
robocopy %srcDIR%\image\vc14projects\Release\JXREncodeLib\x64 %installDIR%\lib *.lib

cd %currDIR%

echo off
pause
echo The batch file is complete.