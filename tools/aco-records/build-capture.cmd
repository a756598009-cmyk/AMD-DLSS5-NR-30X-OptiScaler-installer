@echo off
rem Builds nr_aco_capture.exe (the RADV capture front end of the record producer).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /O2 /EHsc /I"C:\VulkanSDK\1.4.357.0\Include" nr_aco_capture.cpp /Fe:nr_aco_capture.exe || exit /b 1
