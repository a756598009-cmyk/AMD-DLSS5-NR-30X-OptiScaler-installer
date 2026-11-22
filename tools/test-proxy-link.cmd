@echo off
rem Requires an x64 MSVC developer environment and a build from tools\build-optiscaler-fast.cmd.
setlocal EnableExtensions
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-proxy-link.ps1" %*
exit /b %errorlevel%
