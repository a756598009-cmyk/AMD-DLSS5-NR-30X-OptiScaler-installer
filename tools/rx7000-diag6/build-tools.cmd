@echo off
rem build-tools.cmd <out dir>: kreplay.exe, kit 6 (kit 5's with rel, fix and nat as the wide variants).
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul || exit /b 1
cd /d %~dp0
set "VK=C:\VulkanSDK\1.4.357.0"
set "OUT=%~1"
set "OBJ=%OUT%\obj"
if not exist "%OBJ%" mkdir "%OBJ%"
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%"\ /I "%VK%\Include" kreplay.cpp /Fe"%OUT%\kreplay.exe" /link "%VK%\Lib\vulkan-1.lib" delayimp.lib /DELAYLOAD:vulkan-1.dll || exit /b 1
rmdir /s /q "%OBJ%"
echo TOOLS_OK
