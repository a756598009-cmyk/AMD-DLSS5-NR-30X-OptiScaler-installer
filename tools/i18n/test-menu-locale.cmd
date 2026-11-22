@echo off
rem Builds and runs tests\menu_locale_test.cpp, no game and no GPU. The pictures go to exports\menu-locale-test.
rem Needs VS_ROOT as tools\build-optiscaler-fast.cmd does.
setlocal
cd /d "%~dp0..\.."
if not defined VS_ROOT (
  echo FAIL: set VS_ROOT
  exit /b 1
)
call "%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul
if errorlevel 1 exit /b 1
set OUT=exports\menu-locale-test
if not exist %OUT%\obj mkdir %OUT%\obj
set I=OptiScaler\include\imgui
set FLAGS=/nologo /std:c++latest /EHsc /O1 /MD /utf-8 /W1 /DIMGUI_DEFINE_MATH_OPERATORS /IOptiScaler /IOptiScaler\include /IOptiScaler\include\imgui /Iexternal\unordered_dense\include /Iexternal\freetype /Fo%OUT%\obj\ /Fd%OUT%\obj\
if not exist %OUT%\obj\imgui.obj (
  cl %FLAGS% /c %I%\imgui.cpp %I%\imgui_draw.cpp %I%\imgui_widgets.cpp %I%\imgui_tables.cpp %I%\misc\freetype\imgui_freetype.cpp || exit /b 1
)
cl %FLAGS% /c OptiScaler\menu\i18n\MenuLocale.cpp OptiScaler\menu\i18n\MenuLocaleTables.cpp tests\menu_locale_test.cpp || exit /b 1
link /nologo /OUT:%OUT%\menu_locale_test.exe %OUT%\obj\imgui.obj %OUT%\obj\imgui_draw.obj %OUT%\obj\imgui_widgets.obj %OUT%\obj\imgui_tables.obj %OUT%\obj\imgui_freetype.obj %OUT%\obj\MenuLocale.obj %OUT%\obj\MenuLocaleTables.obj %OUT%\obj\menu_locale_test.obj external\freetype\freetype.lib user32.lib imm32.lib shell32.lib /NODEFAULTLIB:libcmt || exit /b 1
%OUT%\menu_locale_test.exe %OUT%
exit /b %errorlevel%
