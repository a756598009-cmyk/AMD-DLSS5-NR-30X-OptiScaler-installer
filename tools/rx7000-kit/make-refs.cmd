@echo off
rem make-refs.cmd: the RX 9070 XT references the kit compares against, made with the kit's own harness and runtime
rem DLL on the FP8 network (dlssnr-amd\shaders), ACO off, same image, sizes, models and 40 frames as kit.ps1.
setlocal
cd /d %~dp0
set "F=..\..\exports\rx7000-kit\OpenNR-RX7000-test\files"
set "R=..\..\exports\rx7000-kit\refwork"
if not exist "%F%\reference" mkdir "%F%\reference"
copy /y "%F%\MochizukiNrRuntime.dll" "%R%\" >nul || exit /b 1
copy /y "%F%\test.png" "%R%\" >nul || exit /b 1
copy /y "%F%\mz_e2e.exe" "%R%\" >nul || exit /b 1
robocopy ..\..\exports\mochizuki-rdna3\dlssnr-amd\shaders "%R%\dlssnr-amd\shaders" /e /purge /njh /njs /nfl /ndl >nul
set MZ_FORCE_RDNA3=
set DLSSNR_MODEL=
set E2E_PAN=
del /q "%F%\reference\*" 2>nul
for %%m in (new old) do for %%s in (1280x720 1920x1080 2560x1440) do call :one %%m %%s
echo REFS_OK
exit /b 0

:one
for /f "tokens=1,2 delims=x" %%a in ("%2") do (set "E2E_W=%%a" & set "E2E_H=%%b")
set E2E_FLAGS=0
if "%1"=="old" set E2E_FLAGS=8192
pushd "%R%"
del /q mochizuki_nr.log 2>nul
.\mz_e2e.exe .\MochizukiNrRuntime.dll . test.png ref_%1_%E2E_H%p.bin 40 1 > ref_%1_%E2E_H%p.txt 2>&1 || (popd & echo RUN_FAIL %1 %2 & exit /b 1)
copy /y mochizuki_nr.log ref_%1_%E2E_H%p.log >nul
popd
rem The new model at 1080p whole; the others their bottom-right 640x360 (the kit is sent over Discord).
set CROP=640 360
if "%1_%E2E_H%"=="new_1080" set CROP=
"%F%\compare.exe" --pack "%R%\ref_%1_%E2E_H%p.bin" %E2E_W% %E2E_H% "%F%\reference\ref_%1_%E2E_H%p.jxr" %CROP% || exit /b 1
echo %1 %2 done
exit /b 0
