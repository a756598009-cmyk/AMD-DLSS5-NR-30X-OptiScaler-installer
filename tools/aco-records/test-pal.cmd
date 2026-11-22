@echo off
rem CPU tests of the PAL adapter: test-pal.cmd <work dir> [--probe2 <tools\aco-records\probe2>] <records dir>...
rem orig = the adapter as of rdna3-k6 (git show rdna3-k6:...), in namespace nr_orig, for the gfx12 identity check;
rem base = the adapter as of dd989104c (gfx11 and gfx10.3 prologues, before the RX 6000 container), in nr_base.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul || exit /b 1
set "W=%~1"
if not exist "%W%" mkdir "%W%"
git -C "%~dp0..\.." show rdna3-k6:third_party/mochizuki/windows/src/core/nr_pal_binary.hpp > "%W%\orig.hpp" || exit /b 1
git -C "%~dp0..\.." show dd989104c:third_party/mochizuki/windows/src/core/nr_pal_binary.hpp > "%W%\base.hpp" || exit /b 1
python -c "import sys;p=sys.argv[1];s=open(p).read().replace('namespace nr::pal','namespace nr_orig::pal');open(sys.argv[2],'w').write(s)" "%W%\orig.hpp" "%W%\nr_pal_binary_orig.hpp" || exit /b 1
python -c "import sys;p=sys.argv[1];s=open(p).read().replace('namespace nr::pal','namespace nr_base::pal');open(sys.argv[2],'w').write(s)" "%W%\base.hpp" "%W%\nr_pal_binary_base.hpp" || exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /I"%~dp0..\..\third_party\mochizuki\windows\src\core" /I"%W%" "%~dp0test_pal_prologue.cpp" /Fo"%W%\\" /Fe"%W%\test_pal_prologue.exe" >"%W%\build.log" || (type "%W%\build.log" & exit /b 1)
shift
"%W%\test_pal_prologue.exe" %1 %2 %3 %4 %5 %6 %7 %8 %9
