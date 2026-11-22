@echo off
rem Local release build matching CI: ordinary Release, multi-slot default, no diagnostics.
setlocal
cd /d "%~dp0.."
if not exist "exports\release-local" mkdir "exports\release-local"
if not exist "exports\release-local\smoke" mkdir "exports\release-local\smoke"
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

echo === regression: AMD host contracts ===
call tools\test-amd-host-contracts.cmd exports\release-local
if errorlevel 1 exit /b 1

echo === regression: shader compatibility ===
call tools\test-shader-compatibility.cmd exports\release-local\shader-tests
if errorlevel 1 exit /b 1

echo === regression: HIP runtime load ===
call tools\test-hip-runtime-load.cmd exports\release-local\hip-tests
if errorlevel 1 exit /b 1

echo === regression: lmxxf evaluate cut ===
call tools\test-lmxxf-evaluate-cut.cmd exports\release-local\eval-tests
if errorlevel 1 exit /b 1

echo === Release build: LmxxfNrRuntime.dll ===
call tools\build-lmxxf-runtime.cmd exports\release-local
if errorlevel 1 exit /b 1
if not exist "exports\lmxxf-runtime" mkdir "exports\lmxxf-runtime"
copy /Y "exports\release-local\LmxxfNrRuntime.dll" "exports\lmxxf-runtime\LmxxfNrRuntime.dll" >nul

echo === Release build (CI-equivalent) ===
"%VS_ROOT%\MSBuild\Current\Bin\MSBuild.exe" "OptiScaler\OptiScaler.vcxproj" /m:4 /t:Build /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:VCToolsVersion=14.44.35207 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:PostBuildEventUseInBuild=false /p:SolutionDir="%CD%/" /p:OutDir="%CD%/exports/release-local/" /p:IntDir="%CD%/exports/release-local/obj/" /v:minimal /nologo /fl /flp:logfile=%CD%\exports\release-local\build.log;verbosity=normal
if errorlevel 1 exit /b 1
if not exist "exports\release-local\OptiScaler.dll" (
  echo FAIL: OptiScaler.dll not produced
  exit /b 1
)
echo BUILD_OK
exit /b 0
