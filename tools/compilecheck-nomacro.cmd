@echo off
rem Compile AmdPreSr.cpp with NO diagnostic macros. Used to prove the
rem stripped (pre-diagnostics) tree still builds before committing it.
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
call "%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44
if errorlevel 1 exit /b 1
if not exist "exports" mkdir "exports"
cl /nologo /c /std:c++latest /EHsc /MD /O2 /DNDEBUG /D_UNICODE /DUNICODE OptiScaler\dlssnr\amd\AmdPreSr.cpp /Foexports\_stripcheck.obj
exit /b %ERRORLEVEL%
