@echo off
setlocal
title OpenNR RX 7000 diagnostic
cd /d "%~dp0"
if not exist "%~dp0files\kit.ps1" (
  echo The files folder is missing. Unzip the whole kit first, then run run-test.bat again.
  pause
  exit /b 1
)
rem A PowerShell 7 module path breaks Windows PowerShell's own modules.
set PSModulePath=
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\kit.ps1"
if errorlevel 1 echo Something went wrong. Please send the result folder and a screenshot of this window.
echo.
pause
