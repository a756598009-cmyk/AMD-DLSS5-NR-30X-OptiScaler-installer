@echo off
rem D3D10 final-image NR in a small D3D10 game that loads the built OptiScaler as dxgi.dll. Not part of the release set:
rem it needs an x64 MSVC developer environment, an RX 9000 GPU and a visible desktop (it opens a window).
rem   tools\test-dx10-final-image.cmd <installed folder> [seconds] [d3d10_1^|d3d10] [full^|r10]
rem <installed folder> is a game folder OptiScaler is installed in with NR without upscaling on: its OptiScaler.ini, the
rem runtime NrBackend names (MochizukiNrRuntime.dll and dlssnr-amd\, or dlssnr_amd_pass1.dll and its weights) and the
rem OptiScaler\ folder are copied beside the test.
setlocal
cd /d "%~dp0.."
if "%~1"=="" goto usage
set "OUT=exports\dx10-final-image"
if not defined OPTI_BUILD set "OPTI_BUILD=exports\release-local"
if not exist "%OUT%" mkdir "%OUT%"
copy /y "%OPTI_BUILD%\OptiScaler.dll" "%OUT%\dxgi.dll" >nul || exit /b 2
copy /y "%~1\OptiScaler.ini" "%OUT%\" >nul || exit /b 2
for %%F in (MochizukiNrRuntime.dll dlssnr_amd_pass1.dll dlssnr_on_amd_weights.bin) do if exist "%~1\%%F" copy /y "%~1\%%F" "%OUT%\" >nul
if exist "%~1\dlssnr-amd" xcopy /y /e /i /q "%~1\dlssnr-amd" "%OUT%\dlssnr-amd" >nul
xcopy /y /e /i /q "%~1\OptiScaler" "%OUT%\OptiScaler" >nul || exit /b 2
if exist "%OUT%\OptiScaler.log" del "%OUT%\OptiScaler.log"
cl /nologo /std:c++20 /EHsc /W3 /utf-8 /MD /O2 tests\dx10_final_image_smoke.cpp /Fe"%OUT%\dx10_final_image_smoke.exe" /Fo"%OUT%\dx10_final_image_smoke.obj" /link d3d10_1.lib d3d10.lib dxgi.lib user32.lib dinput8.lib dxguid.lib
if not %errorlevel%==0 exit /b 2
"%OUT%\dx10_final_image_smoke.exe" %2 %3 %4
exit /b %errorlevel%

:usage
echo usage: %~nx0 ^<installed folder^> [seconds] [d3d10_1^|d3d10] [full^|r10]
exit /b 2
