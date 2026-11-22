@echo off
rem Person protection: capture pacing, the worker IPC against a stand-in worker, the composition on the GPU, the
rem decoders, and with a folder holding the person-model files (onnxruntime.dll, the three models, the CRT DLLs)
rem each model on the given still images (binary PPM), in process and through a freshly built person-worker.exe.
rem Masks go to OUT\masks.
rem   tools\test-person.cmd [person-model folder] [image.ppm ...]
rem Run from an MSVC developer prompt.
setlocal
cd /d "%~dp0.."
set "OUT=exports\person-tests"
if not exist "%OUT%\fixture" mkdir "%OUT%\fixture"
cl /nologo /std:c++20 /EHsc /O2 /utf-8 tests\person_capture_schedule.cpp /Fe"%OUT%\person_capture_schedule.exe" /Fo"%OUT%\person_capture_schedule.obj" || exit /b 1
"%OUT%\person_capture_schedule.exe" || exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 tests\person_worker_fixture.cpp /Fe"%OUT%\fixture\person-worker.exe" /Fo"%OUT%\fixture.obj" || exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 tests\person_worker_ipc.cpp /Fe"%OUT%\person_worker_ipc.exe" /Fo"%OUT%\person_worker_ipc.obj" || exit /b 1
"%OUT%\person_worker_ipc.exe" "%OUT%\fixture" || exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /utf-8 tests\person_partition.cpp /Fe"%OUT%\person_partition.exe" /Fo"%OUT%\person_partition.obj" /link d3d12.lib dxgi.lib d3dcompiler.lib || exit /b 1
"%OUT%\person_partition.exe" || exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /utf-8 tests\person_inference.cpp /Fe"%OUT%\person_inference.exe" /Fo"%OUT%\person_inference.obj" || exit /b 1
set "ASSETS=%~1"
if not defined ASSETS (
  "%OUT%\person_inference.exe"
  exit /b
)
call tools\build-person-worker.cmd "%OUT%\person-model" || exit /b 1
for %%F in (onnxruntime.dll pphumanseg.onnx yolo11n-seg.onnx yunet.onnx msvcp140.dll vcruntime140.dll vcruntime140_1.dll) do (
  copy /y "%ASSETS%\%%F" "%OUT%\person-model\" >nul || exit /b 1
)
if not exist "%OUT%\masks" mkdir "%OUT%\masks"
shift
set "FIRST=%~1"
set "IMAGES="
:images
if "%~1"=="" goto run
set "IMAGES=%IMAGES% "%~1""
shift
goto images
:run
"%OUT%\person_inference.exe" "%OUT%\person-model" "%OUT%\masks" %IMAGES% || exit /b 1
rem The round trip on the GPU: the first still captured, segmented by the worker and composed, with each model.
if defined FIRST "%OUT%\person_partition.exe" "%OUT%" "%FIRST%"
exit /b
