@echo off
rem Runs tests\dx9_present_smoke.cpp against exports\release-local\OptiScaler.dll loaded as d3d9.dll: the menu,
rem the final-image NR, XeFG through the D3D12 presenter, the NR and FG keys and a device Reset, on the GPU.
rem Not part of the release set: it needs the x64 MSVC tools, an AMD GPU with HIP 7 and the danielblnc runtime.
rem   tools\test-dx9-present.cmd [-Case name] [-Runtime folder] [-XeFG folder] [-Lock folder]
rem -Runtime holds dlssnr_amd_pass1.dll, dlssnr_on_amd_weights.bin and dlssnr_on_amd.ini; -XeFG holds
rem libxess_fg.dll and libxell.dll; -Lock names a folder created for the length of each GPU run.
setlocal EnableExtensions
cd /d "%~dp0.."
where cl >nul 2>nul
if not errorlevel 1 goto run
rem A VS_ROOT set beforehand skips vswhere, which lists nothing when the installer's registry is broken.
if not defined VS_ROOT for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%I"
set "VSINSTALLDIR=%VS_ROOT%\"
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
:run
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-dx9-present.ps1" %*
exit /b %errorlevel%
