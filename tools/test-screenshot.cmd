@echo off
rem The screenshot key on D3D11, D3D12 (SDR, scRGB and HDR10), Vulkan, OpenGL and D3D9 (tests\screenshot_smoke.cpp),
rem and on D3D11 a Ctrl+F6 combination, ScreenshotWithMenu=false and the default folder in Pictures: the files written
rem hold the game's frame without the menu or the FPS overlay, which are on screen meanwhile, plus the copy with the menu. Needs exports\release-local\OptiScaler.dll (tools\build-optiscaler-fast.cmd)
rem and an unlocked desktop: the test types into its own window. No NR runtime needed.
rem   tools\test-screenshot.cmd [case ...]   (dx11 dx11-chord dx11-nomenu dx11-pictures dx12 dx12-fp16 dx12-hdr10 vk gl dx9)
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
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-screenshot.ps1" %*
exit /b %errorlevel%
