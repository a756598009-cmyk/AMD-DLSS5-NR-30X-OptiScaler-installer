@echo off
rem Drives a real danielblnc runtime without a game, the way AmdPreSr does: load with the bootstrap
rem isolated, init on the AMD GPU, frames through Record/Execute/Notify in every wait mode, shutdown.
rem Not part of the release set: it needs an x64 MSVC developer environment, an AMD GPU with HIP 7,
rem the runtime (version.dll or a pass DLL) and dlssnr_on_amd_weights.bin.
rem   tools\test-amd-runtime-init.cmd <version.dll> <dlssnr_on_amd_weights.bin> [work folder] [frames per mode]
setlocal
cd /d "%~dp0.."
if "%~2"=="" goto usage
set "AMD_TEST_OUT=exports\amd-runtime-init"
set "AMD_WORK=%~3"
if not defined AMD_WORK set "AMD_WORK=%AMD_TEST_OUT%\work"
if not exist "%AMD_TEST_OUT%" mkdir "%AMD_TEST_OUT%"
cl /nologo /std:c++20 /EHsc /W4 /utf-8 /IOptiScaler\include tests\amd_runtime_init_smoke.cpp /Fe"%AMD_TEST_OUT%\amd_runtime_init_smoke.exe" /Fo"%AMD_TEST_OUT%\amd_runtime_init_smoke.obj" /link OptiScaler\library\detours\detours.lib d3d12.lib dxgi.lib bcrypt.lib
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_runtime_init_smoke.exe" "%~1" "%~2" "%AMD_WORK%" %4
exit /b %errorlevel%
:usage
echo usage: %~nx0 ^<version.dll^> ^<dlssnr_on_amd_weights.bin^> [work folder] [frames per mode]
exit /b 2
