@echo off
rem Requires an x64 MSVC developer environment, an RX 9000 class GPU and a danielblnc runtime with its weights
rem (see the parameters of test-final-image-gate.ps1). The danielblnc cases take the shared GPU lock.
setlocal EnableExtensions
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-final-image-gate.ps1" %*
exit /b %errorlevel%
