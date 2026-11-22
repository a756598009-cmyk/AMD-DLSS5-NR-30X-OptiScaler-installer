@echo off
rem NR at the seam of OptiScaler's native Vulkan upscalers (dlssnr\amd\VulkanSeam): builds tests\vulkan_seam_smoke.cpp,
rem stages it with the built OptiScaler.dll and the NR runtimes of a folder, runs it once per runtime with NR on and
rem once with NR off (VulkanUpscaler=fsr22, FSR 2.2 on Vulkan), and checks that NR changed the upscaled frames.
rem Not part of the release set: it needs an AMD GPU and the runtimes.
rem   tools\test-vulkan-seam.cmd <runtime folder> [mochizuki] [lmxxf] [daniel]
rem The runtime folder holds what the runtimes named want beside OptiScaler.dll: MochizukiNrRuntime.dll and
rem dlssnr-amd\; LmxxfNrRuntime.dll, lmxxf-modules\, shaders\ and native-game-tiled-assets\; dlssnr_amd_pass1.dll and
rem dlssnr_on_amd_weights.bin. Its dxgi.dll, OptiScaler files, INI and logs are not copied. Without names: mochizuki.
rem With UPSCALER set to a w/Dx12 upscaler (fsr22_12) NR runs at the D3D12 seam inside that bridge instead,
rem and the run checks that the Vulkan seam stays out of it.
rem UPSCALER=dlss_12 (or dlss, which must be routed to w/Dx12) with danielblnc 0.8.0 (dlssnr_amd_pass1.dll = the
rem NeuralHub host, dlssnr_amd_host\ with neuralhub_host.dll and nvngx_dlss.dll in the folder): DLSS-SR from the host
rem in the w/Dx12 bridge, checked for no host watchdog firing.
setlocal EnableDelayedExpansion
cd /d "%~dp0.."
if "%~1"=="" goto usage
set "SRC=%~1"
if not defined UPSCALER set "UPSCALER=fsr22"
set "MODE=native"
if not "%UPSCALER:_12=%"=="%UPSCALER%" set "MODE=bridged"
if "%UPSCALER%"=="dlss" set "MODE=bridged"
rem fsr21_12 and ffx_12 now run as the native Vulkan FSR (no FSR w/Dx12 on Vulkan any more): the native checks apply.
if "%UPSCALER%"=="fsr21_12" set "MODE=native"
if "%UPSCALER%"=="ffx_12" set "MODE=native"
set "OUT=exports\vulkan-seam"
if "%MODE%"=="bridged" set "OUT=exports\vulkan-seam-%UPSCALER%"
set "DLL=exports\release-local\OptiScaler.dll"
if not exist "%OUT%" mkdir "%OUT%"
where cl >nul 2>nul && goto compile
if not defined VS_ROOT for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%I"
set "VSINSTALLDIR=%VS_ROOT%\"
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>nul || exit /b 1
:compile
cl /nologo /std:c++20 /EHsc /O2 /W3 /utf-8 /Iexternal\vulkan\include /Iexternal\nvngx_dlss_sdk tests\vulkan_seam_smoke.cpp /Fe"%OUT%\vulkan_seam_smoke.exe" /Fo"%OUT%\vulkan_seam_smoke.obj" /link OptiScaler\library\vulkan\vulkan-1.lib user32.lib
if not %errorlevel%==0 exit /b 1
set "BACKENDS=%~2 %~3 %~4"
if "%~2"=="" set "BACKENDS=mochizuki"
set "FAILED=0"
call :stage off daniel false
call :run off
set "RESULT_OFF=!errorlevel!"
echo NR off: exit !RESULT_OFF!, log %OUT%\off\OptiScaler.log
if not !RESULT_OFF!==0 set "FAILED=1"
for %%B in (%BACKENDS%) do (
  call :stage %%B %%B true
  call :run %%B
  set "RESULT=!errorlevel!"
  echo %%B: exit !RESULT!, log %OUT%\%%B\OptiScaler.log
  if not !RESULT!==0 set "FAILED=1"
  "%OUT%\vulkan_seam_smoke.exe" compare "%OUT%\%%B\frames.bin" "%OUT%\off\frames.bin"
  if not !errorlevel!==0 set "FAILED=1"
)
if "%FAILED%"=="0" (echo ALL PASSED) else (echo SOME FAILED)
exit /b %FAILED%

:stage
set "DIR=%OUT%\%~1"
if not exist "%DIR%" mkdir "%DIR%"
del /q "%DIR%\*.log" "%DIR%\*.bin" 2>nul
del /q "%DIR%\dlssnr_amd_host\*.log" 2>nul
copy /y "%DLL%" "%DIR%\OptiScaler.dll" >nul || exit /b 1
copy /y "%OUT%\vulkan_seam_smoke.exe" "%DIR%\" >nul || exit /b 1
for %%F in ("%SRC%\*.dll" "%SRC%\*.bin") do (
  if /i not "%%~nxF"=="dxgi.dll" if /i not "%%~nxF"=="OptiScaler.dll" copy /y "%%~F" "%DIR%\" >nul
)
for %%D in (dlssnr-amd lmxxf-modules lmxxf-modules-gfx1200 shaders native-game-tiled-assets native-game-tiled-assets.small dlssnr_amd_host) do (
  if exist "%SRC%\%%D" xcopy /e /i /y /d /q "%SRC%\%%D" "%DIR%\%%D" >nul
)
(
  echo [Upscalers]
  echo VulkanUpscaler=%UPSCALER%
  echo [DlssNr]
  echo Enabled=%~3
  echo NrBackend=%~2
  echo [Log]
  echo LogToFile=true
  echo LogLevel=2
) > "%DIR%\OptiScaler.ini"
exit /b 0

:run
powershell -NoProfile -Command "$p = Start-Process -FilePath '%OUT%\%~1\vulkan_seam_smoke.exe' -ArgumentList 'run','frames','%MODE%' -WorkingDirectory '%OUT%\%~1' -NoNewWindow -PassThru; $null = $p.Handle; if (-not $p.WaitForExit(600000)) { $p.Kill(); Write-Output 'FAIL timeout'; exit 124 }; exit $p.ExitCode"
exit /b %errorlevel%

:usage
echo usage: %~nx0 ^<runtime folder^> [mochizuki] [lmxxf] [daniel]
exit /b 2
