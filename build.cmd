@echo off
setlocal
cd /d "%~dp0"
if /i "%VSCMD_ARG_TGT_ARCH%"=="x86" goto build
set "SHIFT2_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%SHIFT2_VSWHERE%" (
  echo Visual Studio C++ Build Tools and the Windows SDK are required.
  exit /b 1
)
for /f "usebackq delims=" %%I in (`"%SHIFT2_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "SHIFT2_VS_ROOT=%%I"
if not defined SHIFT2_VS_ROOT exit /b 1
call "%SHIFT2_VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 exit /b %errorlevel%
:build
nmake /nologo /f Makefile.msvc %*
if errorlevel 1 exit /b %errorlevel%
rem Preserve the artifact paths used by the existing development deploy scripts.
if exist "build\make\dinput8.dll" copy /y "build\make\dinput8.dll" "build\dinput8.dll" >nul
if exist "build\make\shift2-telemetry.dll" (
  if not exist "telemetry\build" mkdir "telemetry\build"
  copy /y "build\make\shift2-telemetry.dll" "telemetry\build\shift2-telemetry.dll" >nul
)
exit /b 0
