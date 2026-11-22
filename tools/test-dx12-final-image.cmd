@echo off
rem Final-image NR on a real D3D12 app: builds tests\dx12_final_image_smoke.cpp, stages it with the
rem built OptiScaler.dll as dxgi.dll, the danielblnc runtime and XeFG, and runs the scenarios.
rem Needs the AMD GPU; each scenario holds wt\locks\gpu while it renders when that folder exists.
rem   tools\test-dx12-final-image.cmd <dlssnr_amd_pass1.dll or version.dll> <dlssnr_on_amd_weights.bin>
rem                                   <folder with libxess_fg.dll and libxell.dll> [scenario args...]
rem Without scenario args it runs: nr, fg, fg --bgra, fg --cpu-ms 6 (a game's CPU time per frame),
rem fg --debug-layer, nr --lifecycle, fg --lifecycle, fg --lifecycle --nr-off-start, nr and fg with
rem --bad-runtime (another DLL in the runtime's place), nr and fg --r10 (a 10-bit SDR backbuffer, which
rem final-image NR takes), fg --hdr10 (the same in HDR10, which it leaves alone), fg with
rem InterpolationCount=auto (3X), fg with UnlockMFG=false (2X), and
rem fg --lifecycle as rdr.exe (Red Dead Redemption's quirk: a plain first swapchain, XeFG's on the next),
rem fg --perf at 3X windowed and borderless with a heavy game frame and at 2X windowed (presented frame rate with
rem XeFG against without it, with NR and without, and XeFG's frames spread over the frame period), the mouse wheel
rem over the open menu, with the game's mouse read through DirectInput in exclusive mode as Red Dead Redemption does,
rem with FG and without, and fg --hitch --reflex (Reflex driven through nvapi as Red Dead Redemption does, and a few
rem very long frames in the middle of play).
rem With scenario args, set SMOKE_EXE=rdr.exe first to run them under that name.
setlocal
cd /d "%~dp0.."
if "%~3"=="" goto usage
set "SMOKE=exports\dx12-smoke"
if not exist "%SMOKE%\OptiScaler" mkdir "%SMOKE%\OptiScaler"
rem A VS_ROOT set beforehand skips vswhere, which lists nothing when the installer's registry is broken.
if not defined VS_ROOT for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%I"
set "VSINSTALLDIR=%VS_ROOT%\"
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W3 /utf-8 tests\dx12_final_image_smoke.cpp /Fe"%SMOKE%\dx12_final_image_smoke.exe" /Fo"%SMOKE%\dx12_final_image_smoke.obj" /link d3dcompiler.lib user32.lib gdi32.lib dbghelp.lib dinput8.lib dxguid.lib
if errorlevel 1 exit /b 1
copy /y "%SMOKE%\dx12_final_image_smoke.exe" "%SMOKE%\rdr.exe" >nul || exit /b 1
copy /y "exports\release-local\OptiScaler.dll" "%SMOKE%\dxgi.dll" >nul || exit /b 1
copy /y "OptiScaler.ini" "%SMOKE%\OptiScaler.template.ini" >nul || exit /b 1
copy /y "%~1" "%SMOKE%\dlssnr_amd_pass1.dll" >nul || exit /b 1
if not exist "%SMOKE%\dlssnr_on_amd_weights.bin" copy /y "%~2" "%SMOKE%\dlssnr_on_amd_weights.bin" >nul || exit /b 1
copy /y "%~3\libxess_fg.dll" "%SMOKE%\OptiScaler\" >nul || exit /b 1
copy /y "%~3\libxell.dll" "%SMOKE%\OptiScaler\" >nul || exit /b 1
copy /y "%~3\amd_fidelityfx_*.dll" "%SMOKE%\OptiScaler\" >nul
set "LOCK="
if exist "%~dp0..\..\locks" set "LOCK=--lock %~dp0..\..\locks\gpu"
set "FAILED=0"
if not defined SMOKE_EXE set "SMOKE_EXE=dx12_final_image_smoke.exe"
set "ARGS="
:args
if "%~4"=="" goto argsdone
set "ARGS=%ARGS% %4"
shift /4
goto args
:argsdone
if defined ARGS (
  call :run %ARGS%
  goto done
)
call :run nr
call :run fg
call :run fg --bgra
call :run fg --cpu-ms 6
call :run fg --debug-layer
call :run nr --lifecycle
call :run fg --lifecycle
call :run fg --lifecycle --nr-off-start
call :run nr --bad-runtime
call :run fg --bad-runtime
call :run nr --r10
call :run fg --r10
call :run fg --hdr10
call :run fg --set XeFG/InterpolationCount=auto
call :run fg --set XeFG/UnlockMFG=false --expect-2x
call :run fg --perf --gpu-load 2000 --cpu-ms 2
call :run fg --perf --borderless --waitable 2 --gpu-load 10000 --cpu-ms 3
call :run fg --perf --gpu-load 2000 --cpu-ms 2 --set XeFG/UnlockMFG=false --expect-2x
call :run fg --hitch --reflex --borderless --waitable 2 --gpu-load 4000 --cpu-ms 3
call :run fg --wheel
call :run fg --wheel --dinput
call :run nr --wheel --dinput
set "SMOKE_EXE=rdr.exe"
call :run fg --lifecycle
:done
if "%FAILED%"=="0" (echo ALL PASSED) else (echo SOME FAILED)
exit /b %FAILED%
:run
echo === %SMOKE_EXE% %*
"%SMOKE%\%SMOKE_EXE%" %* %LOCK%
if errorlevel 1 set "FAILED=1"
set "NAME=%*"
set "NAME=%NAME: =%"
set "NAME=%NAME:/=_%"
if /i "%SMOKE_EXE%"=="rdr.exe" set "NAME=rdr-%NAME%"
for %%L in (OptiScaler.log amd_presr.log dx12_smoke_result.txt) do if exist "%SMOKE%\%%L" copy /y "%SMOKE%\%%L" "%SMOKE%\%NAME%-%%L" >nul
exit /b 0
:usage
echo usage: %~nx0 ^<runtime dll^> ^<dlssnr_on_amd_weights.bin^> ^<XeFG folder^> [scenario args...]
exit /b 2
