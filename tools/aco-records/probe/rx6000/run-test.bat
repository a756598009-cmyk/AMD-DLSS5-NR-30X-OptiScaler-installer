@echo off
setlocal
title OpenNR RX 6000 ACO probe
cd /d "%~dp0"
if not exist "%~dp0files\nr_aco_probe.exe" (
  echo The files folder is missing. Unzip the whole kit first, then run run-test.bat again.
  pause
  exit /b 1
)
echo.
echo OpenNR RX 6000 ACO probe. This takes a few seconds.
echo.
"%~dp0files\nr_aco_probe.exe" "%~dp0files" "%~dp0OpenNR-RX6000-aco-probe-result.txt"
echo.
echo Done. Please send this file:
echo   %~dp0OpenNR-RX6000-aco-probe-result.txt
echo.
pause
