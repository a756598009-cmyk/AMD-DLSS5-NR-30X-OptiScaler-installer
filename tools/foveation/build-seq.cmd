@echo off
rem build-seq.cmd: mz_seq.exe into exports\nr-run (OpenRR's NR hops on a validator dump, OptiScaler\dlssnr\amd\Foveation.h).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul || exit /b 1
cd /d %~dp0
set "OS_=..\..\OptiScaler\dlssnr\backend"
set "OUT=..\..\exports\nr-run"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /Fo"%OUT%\obj"\ /I "%OS_%\lmxxf_runtime" /I "%OS_%\mochizuki_runtime" /I "..\..\OptiScaler" mz_seq.cpp /Fe"%OUT%\mz_seq.exe" /link d3d12.lib dxgi.lib d3dcompiler.lib || exit /b 1
echo BUILD_OK
