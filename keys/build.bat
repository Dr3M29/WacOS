@echo off
rem Builds keys\host\LiquidKeys.exe (the vault the Liquid Keys browser extension talks to).
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if not defined VSDIR (
  echo Visual Studio with "Desktop development with C++" wasn't found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build mkdir build
cl /nologo /std:c++20 /O2 /EHsc /utf-8 /W3 /DUNICODE /D_UNICODE /MT host\LiquidKeys.cpp /Fo:build\ /Fe:host\LiquidKeys.exe
