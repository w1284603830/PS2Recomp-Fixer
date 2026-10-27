@echo off
rem [DoD] Builds the VIF1 UNPACK offline differential test (see harness.cpp) into work\tests\vif1_unpack_diff.
setlocal
set ROOT=%~dp0..\..\..\..
set OUT=%ROOT%\work\tests\vif1_unpack_diff
set OURS=%ROOT%\third_party\src\PS2Recomp\ps2xRuntime\src\lib
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /O2 /EHsc /W3 /I "%OURS%" "%~dp0harness.cpp" /Fo"%OUT%\harness.obj" /Fe"%OUT%\vif1_unpack_diff.exe" || exit /b 1
echo built %OUT%
