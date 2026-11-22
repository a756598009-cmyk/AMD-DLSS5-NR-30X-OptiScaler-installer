@echo off
rem Early list wrap behind Streamline's interposer: tests\lmxxf_interposer_wrap.cpp with tests\sl_interposer_fixture.cpp
rem built as sl.interposer.dll and sl.common.dll. Requires x64 MSVC cl, a D3D12 device and OptiScaler detours.lib.
setlocal
cd /d "%~dp0.."
set "OUT=%~1"
if not defined OUT set "OUT=exports\lmxxf-runtime"
if not exist "%OUT%" mkdir "%OUT%"
set "INC=OptiScaler\include"
set "DETOURS=OptiScaler\library\detours\detours.lib"
cl /nologo /LD /Od /EHsc /W4 /utf-8 tests\sl_interposer_fixture.cpp /Fe"%OUT%\sl.interposer.dll" /Fo"%OUT%\sl_interposer_fixture.obj" /link dxguid.lib
if not %errorlevel%==0 exit /b 1
copy /y "%OUT%\sl.interposer.dll" "%OUT%\sl.common.dll" >nul || exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /utf-8 /I"%INC%" tests\lmxxf_interposer_wrap.cpp /Fe"%OUT%\lmxxf_interposer_wrap.exe" /Fo"%OUT%\lmxxf_interposer_wrap.obj" /link d3d12.lib dxgi.lib dxguid.lib uuid.lib "%DETOURS%"
if not %errorlevel%==0 exit /b 1
"%OUT%\lmxxf_interposer_wrap.exe"
if not %errorlevel%==0 exit /b 1
echo lmxxf_interposer_wrap: PASS
exit /b 0
