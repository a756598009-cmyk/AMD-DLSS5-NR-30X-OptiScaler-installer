@echo off
rem OptiScaler as opengl32.dll in a real OpenGL app (tests\gl_present_smoke.cpp): the menu opens and
rem draws with the Insert key; in nr mode final-image NR runs, in fg mode XeFG presents too.
rem Builds the app with the x64 MSVC tools, stages exports\release-local\OptiScaler.dll and runs it
rem under the GPU lock. nr and fg need a danielblnc runtime folder (dlssnr_amd_pass1.dll and
rem dlssnr_on_amd_weights.bin); fg also needs libxess_fg.dll and libxell.dll in it.
rem threaded renders on a thread that never pumps messages while the window thread moves the window.
rem   tools\test-gl-present.cmd [menu^|nr^|fg] [runtime folder] [threaded]
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
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-gl-present.ps1" %*
exit /b %errorlevel%
