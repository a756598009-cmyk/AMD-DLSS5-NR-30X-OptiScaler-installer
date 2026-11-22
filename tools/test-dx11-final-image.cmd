@echo off
rem D3D11 final-image NR and the D3D11->D3D12 XeFG bridge in a small D3D11 game that loads the
rem built OptiScaler as dxgi.dll. Not part of the release set: it needs an x64 MSVC developer
rem environment, an AMD GPU with HIP 7, a visible desktop (it opens a window and sends key input),
rem and a build with its PDB (tools\build-optiscaler-fast.cmd writes one; OPTI_BUILD names another folder).
rem   tools\test-dx11-final-image.cmd <runtime folder> <release OptiScaler folder> [fg^|nr^|artefacts^|bars^|all] [seconds per phase]
rem <runtime folder> holds dlssnr_amd_pass1.dll and dlssnr_on_amd_weights.bin; <release OptiScaler
rem folder> is the OptiScaler\ folder of a release zip (libxess_fg.dll, libxell.dll, FidelityFX).
rem all runs nr and fg, then fgoff (FG off and on), resize (nine ResizeBuffers with FG on, every other one with the
rem menu open), recreate (the swapchain dropped and made again four times with FG on) and pace (XeFG's present
rem cadence and the frames that reach the screen, and one present per game frame with FG off) and artefacts (black rows
rem in the frames shown with FG on, and NR as strong in them as without FG); artefacts alone runs only that. bars (also
rem in all) reads every frame XeFG presents before the display takes it, in a window over the whole screen: no rows of
rem black, and on a still scene no present that stands out from the one before. It runs twice, once like the 32-bit
rem D3D9 host (BGRA back buffer, tearing) and once like an older D3D11 game (one back buffer, bit-block transfer).
rem all also runs resize on an sRGB back buffer resized with its sRGB format, as Unity games do.
setlocal
cd /d "%~dp0.."
if "%~2"=="" goto usage
set "OUT=exports\dx11-final-image"
if not defined OPTI_BUILD set "OPTI_BUILD=exports\release-local"
set "MODE=%~3"
if not defined MODE set "MODE=all"
if not exist "%OPTI_BUILD%\OptiScaler.pdb" (
  echo FAIL: %OPTI_BUILD%\OptiScaler.pdb missing; build with tools\build-optiscaler-fast.cmd
  exit /b 2
)
if not exist "%OUT%" mkdir "%OUT%"
copy /y "%OPTI_BUILD%\OptiScaler.dll" "%OUT%\dxgi.dll" >nul || exit /b 2
copy /y "%OPTI_BUILD%\OptiScaler.pdb" "%OUT%\OptiScaler.pdb" >nul || exit /b 2
copy /y "OptiScaler.ini" "%OUT%\OptiScaler.template.ini" >nul || exit /b 2
copy /y "%~1\dlssnr_amd_pass1.dll" "%OUT%\" >nul || exit /b 2
if not exist "%OUT%\dlssnr_on_amd_weights.bin" copy /y "%~1\dlssnr_on_amd_weights.bin" "%OUT%\" >nul || exit /b 2
xcopy /y /e /i /q "%~2" "%OUT%\OptiScaler" >nul || exit /b 2
cl /nologo /std:c++20 /EHsc /W3 /utf-8 /MD /O2 tests\dx11_final_image_smoke.cpp /Fe"%OUT%\dx11_final_image_smoke.exe" /Fo"%OUT%\dx11_final_image_smoke.obj" /link /DELAYLOAD:d3d11.dll delayimp.lib d3d11.lib d3dcompiler.lib dbghelp.lib user32.lib gdi32.lib
if not %errorlevel%==0 exit /b 2
set "RESULT=0"
if /i "%MODE%"=="artefacts" goto artefacts
if /i "%MODE%"=="bars" goto bars
if /i not "%MODE%"=="fg" call :Run nr %4
if /i not "%MODE%"=="nr" call :Run fg %4
if /i "%MODE%"=="all" call :Run fgoff %4
if /i "%MODE%"=="all" call :Run resize 2
if /i "%MODE%"=="all" call :RunSrgb
if /i "%MODE%"=="all" call :Run recreate 2
if /i "%MODE%"=="all" call :Run pace %4
:artefacts
if /i "%MODE%"=="all" call :Run artefacts %4
if /i "%MODE%"=="artefacts" call :Run artefacts %4
if /i not "%MODE%"=="all" exit /b %RESULT%
:bars
set "PHASE=%~4"
if not defined PHASE set "PHASE=4"
call :Bars host full=1 tearing=1 bgra=1
call :Bars blt full=1 blt=1
exit /b %RESULT%

:Bars
echo === bars %1
"%OUT%\dx11_final_image_smoke.exe" bars %PHASE% 2 %2 %3 %4
set "CODE=%errorlevel%"
echo exit code %CODE%
if not "%CODE%"=="0" set "RESULT=1"
copy /y "%OUT%\OptiScaler.log" "%OUT%\bars-%1-OptiScaler.log" >nul
copy /y "%OUT%\amd_presr.log" "%OUT%\bars-%1-amd_presr.log" >nul
exit /b 0

:Run
echo === %1
"%OUT%\dx11_final_image_smoke.exe" %1 %2
set "CODE=%errorlevel%"
echo exit code %CODE%
if not "%CODE%"=="0" set "RESULT=1"
copy /y "%OUT%\OptiScaler.log" "%OUT%\%1-OptiScaler.log" >nul
copy /y "%OUT%\amd_presr.log" "%OUT%\%1-amd_presr.log" >nul
exit /b 0

:RunSrgb
echo === resize srgb
"%OUT%\dx11_final_image_smoke.exe" resize 2 2 srgb=1 blt=1
set "CODE=%errorlevel%"
echo exit code %CODE%
if not "%CODE%"=="0" set "RESULT=1"
copy /y "%OUT%\OptiScaler.log" "%OUT%\resize-srgb-OptiScaler.log" >nul
exit /b 0

:usage
echo usage: %~nx0 ^<runtime folder^> ^<release OptiScaler folder^> [fg^|nr^|artefacts^|bars^|all] [seconds per phase]
exit /b 2
