@echo off
rem [DoD] Builds the SPU2 offline differential test (see harness.cpp) in three variants into work\tests\spu2_diff:
rem   spu2_diff_ref.exe   PCSX2 reference (scalar) resampling filters - must be bit-exact with ours
rem   spu2_diff_sse.exe   PCSX2 SSE4.1 resampling filters (what PCSX2 runs on CPUs without AVX2)
rem   spu2_diff_avx2.exe  PCSX2 AVX2 resampling filters (what PCSX2 runs on this PC)
setlocal
set ROOT=%~dp0..\..\..\..
set OUT=%ROOT%\work\tests\spu2_diff
set PCSX2=%ROOT%\third_party\_downloads\pcsx2_646df006a4
set OURS=%ROOT%\third_party\src\PS2Recomp\ps2xIOP\src\emulator\core
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
set COMMON=/nologo /std:c++20 /O2 /EHsc /W3 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS
set PINC=/D_M_X86 /I "%~dp0shim" /I "%PCSX2%\pcsx2" /I "%PCSX2%"
cl %COMMON% /I "%OURS%" /c "%OURS%\iop_spu2.cpp" /Fo"%OUT%\iop_spu2.obj" || exit /b 1
cl %COMMON% /I "%OURS%" /I "%~dp0." /c "%~dp0harness.cpp" /Fo"%OUT%\harness.obj" || exit /b 1
cl %COMMON% %PINC% /D__SSE4_1__ /DDIFF_PCSX2_REFERENCE /c "%~dp0pcsx2_side.cpp" /Fo"%OUT%\pcsx2_ref.obj" || exit /b 1
cl %COMMON% %PINC% /D__SSE4_1__ /c "%~dp0pcsx2_side.cpp" /Fo"%OUT%\pcsx2_sse.obj" || exit /b 1
cl %COMMON% %PINC% /arch:AVX2 /c "%~dp0pcsx2_side.cpp" /Fo"%OUT%\pcsx2_avx2.obj" || exit /b 1
link /nologo "%OUT%\harness.obj" "%OUT%\iop_spu2.obj" "%OUT%\pcsx2_ref.obj" /OUT:"%OUT%\spu2_diff_ref.exe" || exit /b 1
link /nologo "%OUT%\harness.obj" "%OUT%\iop_spu2.obj" "%OUT%\pcsx2_sse.obj" /OUT:"%OUT%\spu2_diff_sse.exe" || exit /b 1
link /nologo "%OUT%\harness.obj" "%OUT%\iop_spu2.obj" "%OUT%\pcsx2_avx2.obj" /OUT:"%OUT%\spu2_diff_avx2.exe" || exit /b 1
echo built %OUT%
