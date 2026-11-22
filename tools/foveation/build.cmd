@echo off
rem build.cmd: mz_fov.exe into exports\fov-run (the harness for OptiScaler\dlssnr\amd\Foveation.h).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul || exit /b 1
cd /d %~dp0
set "OS_=..\..\OptiScaler\dlssnr\backend"
set "OUT=..\..\exports\fov-run"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /Fo"%OUT%\obj"\ /I "%OS_%\lmxxf_runtime" /I "%OS_%\mochizuki_runtime" mz_fov.cpp /Fe"%OUT%\mz_fov.exe" /link d3d12.lib dxgi.lib d3dcompiler.lib windowscodecs.lib ole32.lib || exit /b 1
echo BUILD_OK
