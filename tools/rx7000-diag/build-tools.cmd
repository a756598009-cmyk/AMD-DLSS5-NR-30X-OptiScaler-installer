@echo off
rem build-tools.cmd <out dir>: the kit's tools (harness, image compare, stage compare, pipeline check, system probe).
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul || exit /b 1
cd /d %~dp0
set "VK=C:\VulkanSDK\1.4.357.0"
set "OS_=..\..\OptiScaler\dlssnr\backend"
set "OUT=%~1"
set "OBJ=%OUT%\obj"
if not exist "%OBJ%" mkdir "%OBJ%"
set CL_=/nologo /std:c++20 /EHsc /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%"\
cl %CL_% /I "%OS_%\lmxxf_runtime" /I "%OS_%\mochizuki_runtime" mz_e2e.cpp /Fe"%OUT%\mz_e2e.exe" /link d3d12.lib dxgi.lib windowscodecs.lib ole32.lib || exit /b 1
cl %CL_% compare.cpp /Fe"%OUT%\compare.exe" /link windowscodecs.lib ole32.lib || exit /b 1
cl %CL_% stagecmp.cpp /Fe"%OUT%\stagecmp.exe" || exit /b 1
cl %CL_% /I "%VK%\Include" pipecheck.cpp /Fe"%OUT%\pipecheck.exe" /link "%VK%\Lib\vulkan-1.lib" delayimp.lib /DELAYLOAD:vulkan-1.dll || exit /b 1
cl %CL_% /I "%VK%\Include" vkprobe.cpp /Fe"%OUT%\vkprobe.exe" /link "%VK%\Lib\vulkan-1.lib" dxgi.lib advapi32.lib delayimp.lib /DELAYLOAD:vulkan-1.dll || exit /b 1
if not exist "%OUT%\probe" mkdir "%OUT%\probe"
for %%s in (acc32 acc16 fraga fragb halves kswap) do ..\..\third_party\mochizuki\toolchain\glslang\bin\glslang.exe --target-env vulkan1.3 -o "%OUT%\probe\%%s.spv" "%OS_%\mochizuki_runtime\rdna3_probe\%%s.comp" >nul || exit /b 1
rmdir /s /q "%OBJ%"
echo TOOLS_OK
