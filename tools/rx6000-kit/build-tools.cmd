@echo off
rem build-tools.cmd <out dir>: the RX 6000 kit's tools. vkprobe, pipecheck and the harness are the RX 7000 kit's
rem (tools\rx7000-kit), compare is the RX 7000 diagnostic kit's (quarter/bottom/right references), gpuclock is new.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul || exit /b 1
cd /d %~dp0
set "VK=C:\VulkanSDK\1.4.357.0"
set "OS_=..\..\OptiScaler\dlssnr\backend"
set "K7=..\rx7000-kit"
set "OUT=%~1"
set "OBJ=%OUT%\obj"
if not exist "%OBJ%" mkdir "%OBJ%"
set CL_=/nologo /std:c++20 /EHsc /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%"\
cl %CL_% /I "%OS_%\lmxxf_runtime" /I "%OS_%\mochizuki_runtime" %K7%\mz_e2e.cpp /Fe"%OUT%\mz_e2e.exe" /link d3d12.lib dxgi.lib windowscodecs.lib ole32.lib || exit /b 1
cl %CL_% compare.cpp /Fe"%OUT%\compare.exe" /link windowscodecs.lib ole32.lib || exit /b 1
cl %CL_% /I "%VK%\Include" %K7%\pipecheck.cpp /Fe"%OUT%\pipecheck.exe" /link "%VK%\Lib\vulkan-1.lib" delayimp.lib /DELAYLOAD:vulkan-1.dll || exit /b 1
cl %CL_% /I "%VK%\Include" %K7%\vkprobe.cpp /Fe"%OUT%\vkprobe.exe" /link "%VK%\Lib\vulkan-1.lib" dxgi.lib advapi32.lib delayimp.lib /DELAYLOAD:vulkan-1.dll || exit /b 1
cl %CL_% gpuclock.cpp /Fe"%OUT%\gpuclock.exe" || exit /b 1
rmdir /s /q "%OBJ%"
echo TOOLS_OK
