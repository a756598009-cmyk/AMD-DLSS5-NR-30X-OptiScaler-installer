@echo off
rem The 32-bit OpenGL bridge end to end: builds the proxies and the host (tools\build-dx9-x86.cmd), stages
rem exports\gl-x86 with the opengl32.dll proxy and, in OptiScaler64, the host, exports\release-local\OptiScaler.dll and
rem what an installed folder holds, then runs tests\gl_x86_smoke.cpp built for x86 there, which must end within
rem 60 s of its run (no hang on resize, fullscreen or exit). Needs an AMD GPU and a visible desktop (it opens a window
rem and presses the menu key in it). Options as for the program (wgl, full, msaa), and memory: frames cross through
rem memory ([GlX86] SharedMemory=true).
rem   tools\test-gl-x86.cmd <installed folder> [seconds] [wgl] [full] [msaa] [memory]
rem <installed folder> is a game folder OptiScaler is installed in with NR without upscaling on, as for
rem tools\test-dx10-x86.cmd.
setlocal
cd /d "%~dp0.."
if "%~1"=="" goto usage
call tools\build-dx9-x86.cmd || exit /b 2
set "OUT=exports\gl-x86"
call :stage "%~1" || exit /b 2
for %%A in (%~3 %~4 %~5 %~6) do if /i "%%A"=="memory" (echo.& echo [GlX86]& echo SharedMemory=true) >> "%OUT%\OptiScaler64\OptiScaler.ini"
if not defined VS_ROOT for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath 2^>nul`) do set "VS_ROOT=%%I"
if not defined VS_ROOT if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC" set "VS_ROOT=%ProgramFiles%\Microsoft Visual Studio\18\Community"
call "%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" x86 -vcvars_ver=14.44 >nul || exit /b 2
cl /nologo /std:c++20 /EHsc /W3 /utf-8 /MD /O2 tests\gl_x86_smoke.cpp /Fe"%OUT%\gl_x86_smoke.exe" /Fo"%OUT%\obj32\gl_x86_smoke.obj" /link opengl32.lib user32.lib gdi32.lib
if not %errorlevel%==0 exit /b 2
set "SECONDS=%~2"
if "%SECONDS%"=="" set "SECONDS=40"
call :run "%OUT%\gl_x86_smoke.exe" "%SECONDS% %~3 %~4 %~5 %~6"
exit /b %errorlevel%

rem The host, OptiScaler and the installed folder's runtime in %OUT%\OptiScaler64, with fresh logs.
:stage
set "HOST=%OUT%\OptiScaler64"
if not exist "%HOST%" mkdir "%HOST%"
copy /y "exports\dx9-x86\OptiScaler64\OptiScalerDx9Host.exe" "%HOST%\" >nul || exit /b 2
copy /y "exports\release-local\OptiScaler.dll" "%HOST%\" >nul || exit /b 2
copy /y "%~1\OptiScaler.ini" "%HOST%\" >nul || exit /b 2
for %%F in (MochizukiNrRuntime.dll dlssnr_amd_pass1.dll dlssnr_on_amd_weights.bin) do if exist "%~1\%%F" copy /y "%~1\%%F" "%HOST%\" >nul
if exist "%~1\dlssnr-amd" xcopy /y /e /i /q "%~1\dlssnr-amd" "%HOST%\dlssnr-amd" >nul
xcopy /y /e /i /q "%~1\OptiScaler" "%HOST%\OptiScaler" >nul || exit /b 2
for %%F in (OptiScaler.log OptiScaler.previous.log dx9-host.log dxgi-x86.log opengl32-x86.log) do if exist "%HOST%\%%F" del "%HOST%\%%F"
exit /b 0

rem Runs the program and ends it if it is still there 60 s after its own run time.
:run
powershell -NoProfile -Command "$p = Start-Process -FilePath '%~1' -ArgumentList '%~2' -NoNewWindow -PassThru; $null = $p.Handle; if (-not $p.WaitForExit((%SECONDS% + 60) * 1000)) { $p.Kill(); Write-Output 'FAIL the program hung and was ended'; exit 3 }; exit $p.ExitCode"
exit /b %errorlevel%

:usage
echo usage: %~nx0 ^<installed folder^> [seconds] [wgl] [full] [msaa] [memory]
exit /b 2
