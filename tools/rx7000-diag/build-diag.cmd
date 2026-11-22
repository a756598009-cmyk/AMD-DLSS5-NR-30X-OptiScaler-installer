@echo off
rem build-diag.cmd: the diagnostic MochizukiNrRuntime.dll (0.5.8 + MZ_STAGE_DUMP) into exports\mochizuki-diag.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul || exit /b 1
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
call "%~dp0..\build-mochizuki-runtime.cmd" exports\mochizuki-diag
