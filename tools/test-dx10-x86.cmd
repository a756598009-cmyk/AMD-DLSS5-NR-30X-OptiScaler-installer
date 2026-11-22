@echo off
rem The 32-bit D3D10 bridge end to end: builds the proxies and the host (tools\build-dx9-x86.cmd), stages
rem exports\dxgi-x86 with the dxgi.dll proxy and, in OptiScaler64, the host, exports\release-local\OptiScaler.dll and
rem what an installed folder holds, then runs tests\dx10_final_image_smoke.cpp built for x86 there. Needs an RX 9000
rem GPU and a visible desktop (it opens a window).
rem   tools\test-dx10-x86.cmd <installed folder> [seconds] [d3d10_1^|d3d10] [full]
rem <installed folder> is a game folder OptiScaler is installed in with NR without upscaling on, as for
rem tools\test-dx10-final-image.cmd.
setlocal
cd /d "%~dp0.."
if "%~1"=="" goto usage
call tools\build-dx9-x86.cmd || exit /b 2
set "OUT=exports\dxgi-x86"
set "HOST=%OUT%\OptiScaler64"
if not exist "%HOST%" mkdir "%HOST%"
copy /y "exports\dx9-x86\OptiScaler64\OptiScalerDx9Host.exe" "%HOST%\" >nul || exit /b 2
copy /y "exports\release-local\OptiScaler.dll" "%HOST%\" >nul || exit /b 2
copy /y "%~1\OptiScaler.ini" "%HOST%\" >nul || exit /b 2
for %%F in (MochizukiNrRuntime.dll dlssnr_amd_pass1.dll dlssnr_on_amd_weights.bin) do if exist "%~1\%%F" copy /y "%~1\%%F" "%HOST%\" >nul
if exist "%~1\dlssnr-amd" xcopy /y /e /i /q "%~1\dlssnr-amd" "%HOST%\dlssnr-amd" >nul
xcopy /y /e /i /q "%~1\OptiScaler" "%HOST%\OptiScaler" >nul || exit /b 2
for %%F in (OptiScaler.log dx9-host.log dxgi-x86.log) do if exist "%HOST%\%%F" del "%HOST%\%%F"
if not defined VS_ROOT for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath 2^>nul`) do set "VS_ROOT=%%I"
if not defined VS_ROOT if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC" set "VS_ROOT=%ProgramFiles%\Microsoft Visual Studio\18\Community"
call "%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" x86 -vcvars_ver=14.44 >nul || exit /b 2
cl /nologo /std:c++20 /EHsc /W3 /utf-8 /MD /O2 tests\dx10_final_image_smoke.cpp /Fe"%OUT%\dx10_final_image_smoke.exe" /Fo"%OUT%\obj32\dx10_final_image_smoke.obj" /link d3d10_1.lib d3d10.lib user32.lib dinput8.lib dxguid.lib
if not %errorlevel%==0 exit /b 2
"%OUT%\dx10_final_image_smoke.exe" %2 %3 %4
exit /b %errorlevel%

:usage
echo usage: %~nx0 ^<installed folder^> [seconds] [d3d10_1^|d3d10] [full]
exit /b 2
