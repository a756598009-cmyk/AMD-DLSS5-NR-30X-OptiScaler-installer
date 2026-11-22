@echo off
rem Requires an x64 MSVC developer environment and Python 3 (standard library only).
setlocal
cd /d "%~dp0.."
set "AMD_TEST_OUT=%~1"
if not defined AMD_TEST_OUT set "AMD_TEST_OUT=exports\amd-host-tests"
for %%I in ("%AMD_TEST_OUT%") do set "AMD_TEST_OUT=%%~fI"
if not exist "%AMD_TEST_OUT%" mkdir "%AMD_TEST_OUT%"
if not defined AMD_TEST_PYTHON set "AMD_TEST_PYTHON=python"
cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\amd_dynamic_scale.cpp /Fe"%AMD_TEST_OUT%\amd_dynamic_scale.exe" /Fo"%AMD_TEST_OUT%\amd_dynamic_scale.obj"
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_dynamic_scale.exe"
if not %errorlevel%==0 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 tests\nr_backend_selector.cpp /Fe"%AMD_TEST_OUT%\nr_backend_selector.exe" /Fo"%AMD_TEST_OUT%\nr_backend_selector.obj"
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\nr_backend_selector.exe"
if not %errorlevel%==0 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 tests\amd_submission_state.cpp /Fe"%AMD_TEST_OUT%\amd_submission_state.exe" /Fo"%AMD_TEST_OUT%\amd_submission_state.obj"
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_submission_state.exe"
if not %errorlevel%==0 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\amd_graphics_snapshot.cpp /Fe"%AMD_TEST_OUT%\amd_graphics_snapshot.exe" /Fo"%AMD_TEST_OUT%\amd_graphics_snapshot.obj"
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_graphics_snapshot.exe"
if not %errorlevel%==0 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\amd_reno_composition.cpp /Fe"%AMD_TEST_OUT%\amd_reno_composition.exe" /Fo"%AMD_TEST_OUT%\amd_reno_composition.obj" /link d3d12.lib dxgi.lib d3dcompiler.lib
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_reno_composition.exe"
if not %errorlevel%==0 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\amd_graphics_tracker.cpp /Fe"%AMD_TEST_OUT%\amd_graphics_tracker.exe" /Fo"%AMD_TEST_OUT%\amd_graphics_tracker.obj"
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_graphics_tracker.exe"
if not %errorlevel%==0 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\amd_stall_watch.cpp /Fe"%AMD_TEST_OUT%\amd_stall_watch.exe" /Fo"%AMD_TEST_OUT%\amd_stall_watch.obj"
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_stall_watch.exe"
if not %errorlevel%==0 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\amd_graphics_restore.cpp /Fe"%AMD_TEST_OUT%\amd_graphics_restore.exe" /Fo"%AMD_TEST_OUT%\amd_graphics_restore.obj"
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_graphics_restore.exe"
if not %errorlevel%==0 exit /b 1
rem D3 GPU scenario execution is intentionally omitted; validation continues in real games.
cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\amd_graphics_restore_dx12.cpp /Fe"%AMD_TEST_OUT%\amd_graphics_restore_dx12.exe" /Fo"%AMD_TEST_OUT%\amd_graphics_restore_dx12.obj"
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_graphics_restore_dx12.exe"
if not %errorlevel%==0 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\amd_graphics_invocation.cpp /Fe"%AMD_TEST_OUT%\amd_graphics_invocation.exe" /Fo"%AMD_TEST_OUT%\amd_graphics_invocation.obj"
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_graphics_invocation.exe"
if not %errorlevel%==0 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 /IOptiScaler\include tests\amd_graphics_native_hooks.cpp /Fe"%AMD_TEST_OUT%\amd_graphics_native_hooks.exe" /Fo"%AMD_TEST_OUT%\amd_graphics_native_hooks.obj" /link OptiScaler\library\detours\detours.lib
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_graphics_native_hooks.exe"
if not %errorlevel%==0 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /utf-8 /IOptiScaler\include tests\amd_runtime_host_load.cpp /Fe"%AMD_TEST_OUT%\amd_runtime_host_load.exe" /Fo"%AMD_TEST_OUT%\amd_runtime_host_load.obj" /link OptiScaler\library\detours\detours.lib
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_PYTHON%" -B tests\amd_runtime_host_fixture.py "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture.dll"
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_runtime_host_load.exe" "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture.dll"
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_PYTHON%" -B tests\amd_runtime_host_fixture.py "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture_040.dll" --layout 0.4.0
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_runtime_host_load.exe" "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture_040.dll" --layout=0.4.0
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_PYTHON%" -B tests\amd_runtime_host_fixture.py "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture_041.dll" --layout 0.4.1
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_runtime_host_load.exe" "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture_041.dll" --layout=0.4.1
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_PYTHON%" -B tests\amd_runtime_host_fixture.py "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture_042.dll" --layout 0.4.2
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_runtime_host_load.exe" "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture_042.dll" --layout=0.4.2
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_PYTHON%" -B tests\amd_runtime_host_fixture.py "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture_043.dll" --layout 0.4.3
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_runtime_host_load.exe" "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture_043.dll" --layout=0.4.3
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_PYTHON%" -B tests\amd_runtime_host_fixture.py "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture_050.dll" --layout 0.5.0
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_runtime_host_load.exe" "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture_050.dll" --layout=0.5.0
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_PYTHON%" -B tests\amd_runtime_host_fixture.py "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture_051.dll" --layout 0.5.1
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_runtime_host_load.exe" "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture_051.dll" --layout=0.5.1
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_PYTHON%" -B tests\amd_runtime_host_fixture.py "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture_060.dll" --layout 0.6.0
if not %errorlevel%==0 exit /b 1
"%AMD_TEST_OUT%\amd_runtime_host_load.exe" "%AMD_TEST_OUT%\amd_runtime_bootstrap_fixture_060.dll" --layout=0.6.0
if not %errorlevel%==0 exit /b 1
rem AmdLayout.h and the bootstrap contracts against the runtime binaries. None ship with the
rem repository: set AMD_RUNTIME_DIR to a folder holding them (version.dll or dlssnr_amd_pass*.dll,
rem searched recursively). Without it only the headers are checked.
if not defined AMD_RUNTIME_DIR goto layout_headers_only
"%AMD_TEST_PYTHON%" -B tests\amd_layout_binary_check.py "%AMD_RUNTIME_DIR%"
exit /b %errorlevel%
:layout_headers_only
"%AMD_TEST_PYTHON%" -B tests\amd_layout_binary_check.py
exit /b %errorlevel%
