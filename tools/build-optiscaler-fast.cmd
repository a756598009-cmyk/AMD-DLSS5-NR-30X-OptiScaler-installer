@echo off
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
rem The PDB beside the DLL lets tests\dx11_final_image_smoke.cpp reach the settings the menu writes. The DLL
rem names it without the local path. The map lets tools\test-proxy-link.cmd check the link.
set "LINK=/DEBUG:FULL /PDBALTPATH:%%_PDB%% /MAP"
"%VS_ROOT%\MSBuild\Current\Bin\MSBuild.exe" "OptiScaler\OptiScaler.vcxproj" /m:4 /t:Build /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:VCToolsVersion=14.44.35207 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:PostBuildEventUseInBuild=false /p:SolutionDir="%CD%/" /p:OutDir="%CD%/exports/release-local/" /p:IntDir="%CD%/exports/release-local/obj/" /v:minimal /nologo
if errorlevel 1 exit /b 1
echo BUILD_OK exports\release-local\OptiScaler.dll
exit /b 0
