@echo off
rem build-kit.cmd: the tools and the runtime's files into exports\rx7000-kit\OpenNR-RX7000-test\files (references: make-refs.cmd).
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul || exit /b 1
cd /d %~dp0
set "VK=C:\VulkanSDK\1.4.357.0"
set "OS_=..\..\OptiScaler\dlssnr\backend"
set "GLSLANG=..\..\third_party\mochizuki\toolchain\glslang\bin\glslang.exe"
set "RT=..\..\exports\mochizuki-rdna3"
set "KIT=..\..\exports\rx7000-kit\OpenNR-RX7000-test"
set "F=%KIT%\files"
set "OBJ=..\..\exports\rx7000-kit\obj"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%F%\probe" mkdir "%F%\probe"
set CL_=/nologo /std:c++20 /EHsc /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%"\
cl %CL_% /I "%VK%\Include" vkprobe.cpp /Fe"%F%\vkprobe.exe" /link "%VK%\Lib\vulkan-1.lib" dxgi.lib advapi32.lib delayimp.lib /DELAYLOAD:vulkan-1.dll || exit /b 1
cl %CL_% /I "%VK%\Include" pipecheck.cpp /Fe"%F%\pipecheck.exe" /link "%VK%\Lib\vulkan-1.lib" delayimp.lib /DELAYLOAD:vulkan-1.dll || exit /b 1
cl %CL_% /I "%OS_%\lmxxf_runtime" /I "%OS_%\mochizuki_runtime" mz_e2e.cpp /Fe"%F%\mz_e2e.exe" /link d3d12.lib dxgi.lib windowscodecs.lib ole32.lib || exit /b 1
cl %CL_% compare.cpp /Fe"%F%\compare.exe" /link windowscodecs.lib ole32.lib || exit /b 1
for %%s in (acc32 acc16 fraga fragb halves kswap) do "%GLSLANG%" --target-env vulkan1.3 -o "%F%\probe\%%s.spv" ..\..\OptiScaler\dlssnr\backend\mochizuki_runtime\rdna3_probe\%%s.comp >nul || exit /b 1
copy /y "%RT%\MochizukiNrRuntime.dll" "%F%\" >nul || exit /b 1
robocopy "%RT%\dlssnr-amd\shaders-rdna3" "%F%\dlssnr-amd\shaders-rdna3" /e /purge /njh /njs /nfl /ndl >nul
robocopy "%RT%\dlssnr-amd\shaders\renodx" "%F%\dlssnr-amd\shaders\renodx" /e /purge /njh /njs /nfl /ndl >nul
copy /y E:\Projetos\opti_rr_dlss5\arrumar_cor\original_sem_nr.png "%F%\test.png" >nul || exit /b 1
copy /y kit.ps1 "%F%\" >nul || exit /b 1
copy /y run-test.bat "%KIT%\" >nul || exit /b 1
copy /y README.txt "%KIT%\" >nul || exit /b 1
echo BUILD_OK
