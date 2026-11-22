@echo off
setlocal
title OpenNR RX 6000 ACO probe 2
cd /d "%~dp0"
if not exist "%~dp0files\nr_aco_probe2.exe" (
  echo The files folder is missing. Unzip the whole kit first, then run run-test.bat again.
  pause
  exit /b 1
)
set "OUT=%~dp0OpenNR-RX6000-aco-probe2-result"
set "ZIP=%~dp0OpenNR-RX6000-aco-probe2-result.zip"
if exist "%OUT%" rmdir /s /q "%OUT%"
echo.
echo OpenNR RX 6000 ACO probe 2.
echo.
echo Step 1 of 2: the driver compiles a few shaders and we save what it made. Up to a minute.
"%~dp0files\nr_aco_probe2.exe" capture "%~dp0files" "%OUT%"
call :pack
echo.
echo Step 2 of 2: our test code in the driver's format. A few seconds.
echo If the screen goes black for a moment here, that is fine: the file below is already saved.
"%~dp0files\nr_aco_probe2.exe" variants "%~dp0files" "%OUT%"
call :pack
echo.
echo Done. Please send this file:
echo   %ZIP%
echo.
pause
exit /b 0

:pack
if exist "%ZIP%" del /q "%ZIP%"
"%SystemRoot%\System32\tar.exe" -a -c -f "%ZIP%" -C "%OUT%" . >nul 2>&1
if not exist "%ZIP%" powershell -NoProfile -Command "Compress-Archive -LiteralPath (Get-ChildItem -LiteralPath $env:OUT).FullName -DestinationPath $env:ZIP -Force" >nul 2>&1
exit /b 0
