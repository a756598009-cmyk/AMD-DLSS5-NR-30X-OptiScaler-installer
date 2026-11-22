@echo off
rem build-probe2.cmd <out dir>: nr_aco_probe2.exe in <out>, plus <out>\base (nr_aco_probe's build: probe_a/b.spv and
rem their ACO records for gfx1201, navi21-24 and navi31, see ..\probe\build-probe.cmd).
setlocal
set "HERE=%~dp0"
set "OUT=%~1"
call "%HERE%..\probe\build-probe.cmd" "%OUT%\base" || exit /b 1
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul || exit /b 1
set "CORE=%HERE%..\..\..\third_party\mochizuki\windows\src\core"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /std:c++17 /EHsc /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /I "C:\VulkanSDK\1.4.357.0\Include" /I "%CORE%" /Fo"%OUT%\obj"\ "%HERE%nr_aco_probe2.cpp" /Fe"%OUT%\nr_aco_probe2.exe" /link "C:\VulkanSDK\1.4.357.0\Lib\vulkan-1.lib" || exit /b 1
rmdir /s /q "%OUT%\obj"
echo PROBE2_OK
