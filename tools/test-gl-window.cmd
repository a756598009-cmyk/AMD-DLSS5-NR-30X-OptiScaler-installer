@echo off
rem What a game does to its window (tests\gl_window_smoke.cpp), with OptiScaler as opengl32.dll and
rem final-image NR on, XeFG too with fg. recreate destroys the window and context halfway and opens a
rem new window at the same size; resize goes through 150 client sizes, 8 frames apart; two and twosame
rem present to a second window (640x360, or 1280x720) with the same context and open the menu. Builds the app
rem with the x64 MSVC tools, stages exports\release-local\OptiScaler.dll and runs it under the GPU
rem lock. The runtime folder holds dlssnr_amd_pass1.dll and dlssnr_on_amd_weights.bin, and for fg
rem libxess_fg.dll and libxell.dll.
rem   tools\test-gl-window.cmd recreate^|resize^|two^|twosame ^<runtime folder^> [fg]
setlocal
cd /d "%~dp0.."
rem A VS_ROOT set beforehand skips vswhere, which lists nothing when the installer's registry is broken.
if not defined VS_ROOT for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%I"
if not defined VS_ROOT (
  echo FAIL: no Visual Studio with the x64 C++ tools found
  exit /b 1
)
set "VSINSTALLDIR=%VS_ROOT%\"
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-gl-window.ps1" %*
exit /b %errorlevel%
