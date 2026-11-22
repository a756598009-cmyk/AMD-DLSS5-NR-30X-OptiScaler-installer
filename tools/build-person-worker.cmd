@echo off
rem Build person-worker.exe (person protection, NrPersonPartition) into a person-model folder.
rem The models, onnxruntime.dll and the CRT DLLs beside it come from the installer (third_party\onnxruntime\README.md).
setlocal
cd /d "%~dp0.."
set "PERSON_OUT=%~1"
if not defined PERSON_OUT set "PERSON_OUT=exports\release-local\person-model"
if not exist "%PERSON_OUT%" mkdir "%PERSON_OUT%"
where cl.exe >nul 2>&1
if not errorlevel 1 goto build
rem A VS_ROOT set beforehand skips vswhere, as in build-optiscaler-fast.cmd.
if not defined VS_ROOT for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%I"
if not defined VS_ROOT (
  echo FAIL: cl.exe not found; run from an MSVC developer prompt or set VS_ROOT
  exit /b 1
)
set "VSINSTALLDIR=%VS_ROOT%\"
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul || exit /b 1
:build
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX ^
  OptiScaler\dlssnr\person\worker\person_worker.cpp ^
  /Fo"%PERSON_OUT%\person-worker.obj" /Fe"%PERSON_OUT%\person-worker.exe" ^
  /link /SUBSYSTEM:WINDOWS shell32.lib
if errorlevel 1 (
  echo FAIL: person-worker.exe
  exit /b 1
)
del "%PERSON_OUT%\person-worker.obj" >nul 2>&1
echo BUILD_OK %PERSON_OUT%\person-worker.exe
exit /b 0
