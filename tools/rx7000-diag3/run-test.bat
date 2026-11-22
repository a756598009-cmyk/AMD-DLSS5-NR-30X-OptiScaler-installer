@echo off
setlocal
title OpenNR RX 7000 diagnostic 3
cd /d "%~dp0"
if not exist "%~dp0files\kreplay.exe" (
  echo The files folder is missing. Unzip the whole kit first, then run run-test.bat again.
  pause
  exit /b 1
)
if exist "%~dp0result" rmdir /s /q "%~dp0result"
mkdir "%~dp0result"
echo OpenNR RX 7000 diagnostic 3: about 5 to 10 minutes. Please leave the PC alone until it says Done.
echo.
"%~dp0files\kreplay.exe" kit "%~dp0files" "%~dp0result"
if errorlevel 1 echo Something went wrong. Please send the result folder and a screenshot of this window.
rem A PowerShell 7 module path breaks Windows PowerShell's own modules.
set PSModulePath=
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$z = Join-Path '%~dp0' ('OpenNR-RX7000-diag3-result-' + (Get-Date -Format 'yyyyMMdd-HHmm') + '.zip'); Add-Type -AssemblyName System.IO.Compression.FileSystem; [IO.Compression.ZipFile]::CreateFromDirectory('%~dp0result', $z); Write-Host ''; Write-Host ('Done. Please send this file: ' + $z); Start-Process explorer.exe ('/select,\"' + $z + '\"')"
echo.
pause
