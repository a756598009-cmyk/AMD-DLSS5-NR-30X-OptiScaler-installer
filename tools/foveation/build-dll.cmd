@echo off
rem build-dll.cmd [runtime]: OptiScaler.dll into exports\fov-build (as wt-mega's release builds), and with "runtime" also
rem MochizukiNrRuntime.dll and its shaders into exports\fov-runtime. Needs junctions.cmd on.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul || exit /b 1
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
cd /d "%~dp0..\.."
if /i "%1"=="runtime" (
  call tools\build-mochizuki-runtime.cmd exports\fov-runtime || exit /b 1
  echo RUNTIME_OK
  exit /b 0
)
if not exist exports\fov-build mkdir exports\fov-build
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "OptiScaler\OptiScaler.vcxproj" /m:4 /t:Build /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:VCToolsVersion=14.44.35207 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:PostBuildEventUseInBuild=false /p:SolutionDir="%CD%/" /p:OutDir="%CD%/exports/fov-build/" /p:IntDir="%CD%/exports/fov-build/obj/" /v:minimal /nologo /fl /flp:logfile=%CD%\exports\fov-build\build.log;verbosity=normal || exit /b 1
echo OPTI_OK
