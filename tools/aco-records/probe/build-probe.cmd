@echo off
rem build-probe.cmd <out dir>: nr_aco_probe.exe, probe_a/b.spv and their ACO records for gfx1201 (this PC's check),
rem gfx1030/1031/1032/1034 (RX 6000) and gfx1100 (RX 7000) in <out>\records\<machine>\.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul || exit /b 1
set "HERE=%~dp0"
set "OUT=%~1"
set "CORE=%HERE%..\..\..\third_party\mochizuki\windows\src\core"
set "GLSLANG=%HERE%..\..\..\third_party\mochizuki\toolchain\glslang\bin\glslang.exe"
set "TOOLS=E:\Projetos\opti_rr_dlss5\aco-tools"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
for %%s in (probe_a probe_b) do "%GLSLANG%" -V --target-env vulkan1.3 -o "%OUT%\%%s.spv" "%HERE%%%s.comp" >nul || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /I "C:\VulkanSDK\1.4.357.0\Include" /I "%CORE%" /Fo"%OUT%\obj"\ "%HERE%nr_aco_probe.cpp" /Fe"%OUT%\nr_aco_probe.exe" /link "C:\VulkanSDK\1.4.357.0\Lib\vulkan-1.lib" || exit /b 1
rmdir /s /q "%OUT%\obj"
if not exist "%OUT%\in" mkdir "%OUT%\in"
copy /y "%OUT%\probe_*.spv" "%OUT%\in\" >nul
for %%f in (gfx1201 navi21 navi22 navi23 navi24 navi31) do (
  python "%TOOLS%\scripts\produce.py" %%f "%OUT%\in" "%OUT%\rec-%%f" --all --jobs 2 || exit /b 1
  if not exist "%OUT%\records\%%f" mkdir "%OUT%\records\%%f"
  copy /y "%OUT%\rec-%%f\records\*.nrp" "%OUT%\records\%%f\" >nul
)
echo PROBE_OK
