@echo off
rem The 32-bit D3D9 bridge end to end: builds the proxy, the host and the x86 smoke program
rem (tools\build-dx9-x86.cmd), stages them in exports\dx9-x86 with exports\release-local\OptiScaler.dll,
rem the danielblnc runtime, its weights and XeFG, then runs every case of tools\test-dx9-x86.ps1 under
rem the GPU lock. Needs an AMD GPU, a built OptiScaler.dll and the runtime files (defaults: copies from
rem The Sims 4's Bin folder; pass -Runtime, -Weights, -XeFG to the .ps1 to change them).
rem   tools\test-dx9-x86.cmd [-Only case,case]
setlocal
cd /d "%~dp0.."
call tools\build-dx9-x86.cmd
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\test-dx9-x86.ps1 %*
exit /b %errorlevel%
