@echo off
rem Builds Liquid.exe. Needs Visual Studio 2022 or later (Community is fine) with
rem "Desktop development with C++", which brings the compiler and the Windows SDK.
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if not defined VSDIR (
  echo Visual Studio with "Desktop development with C++" wasn't found. Install it, then run build.bat again.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build mkdir build
cl /nologo /std:c++20 /O2 /EHsc /utf-8 /W3 /DUNICODE /D_UNICODE /MT src\*.cpp /Fo:build\ /Fe:Liquid.exe ^
  /link /SUBSYSTEM:WINDOWS d3d11.lib dxgi.lib d2d1.lib dwrite.lib windowscodecs.lib dcomp.lib d3dcompiler.lib ^
  dwmapi.lib shell32.lib ole32.lib user32.lib gdi32.lib version.lib shlwapi.lib shcore.lib powrprof.lib ^
  advapi32.lib wlanapi.lib iphlpapi.lib windowsapp.lib dxguid.lib bcrypt.lib secur32.lib wtsapi32.lib pdh.lib
