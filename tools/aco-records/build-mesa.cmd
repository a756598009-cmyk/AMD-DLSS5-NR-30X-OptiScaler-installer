@echo off
rem Builds Mesa's RADV (null winsys, ACO, no LLVM) on Windows for the NRPAL record producer.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul || exit /b 1
set "PATH=C:\Users\matheus\AppData\Roaming\Python\Python314\Scripts;%PATH%"
cd /d "%~dp0mesa"
if not exist "%~dp0build\radv\build.ninja" (
  meson setup "%~dp0build\radv" --buildtype=release -Db_vscrt=md --default-library=static ^
    -Dplatforms=windows -Dvulkan-drivers=amd -Dgallium-drivers= -Dopengl=false -Degl=disabled -Dgles1=disabled -Dgles2=disabled ^
    -Dllvm=disabled -Damd-use-llvm=false -Dshared-glapi=disabled -Dzstd=disabled -Dvalgrind=disabled -Dlibunwind=disabled ^
    -Dbuild-tests=false -Dvulkan-layers= -Dtools= -Dvideo-codecs= || exit /b 1
)
ninja -C "%~dp0build\radv" %* || exit /b 1
