@echo off
setlocal
cd /d "%~dp0.."
set "NYXEN_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%NYXEN_VSWHERE%" exit /b 1
for /f "usebackq tokens=*" %%i in (`"%NYXEN_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "NYXEN_VS=%%i"
if not defined NYXEN_VS exit /b 1
call "%NYXEN_VS%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
set "NYXEN_CMAKE=%NYXEN_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "PATH=%NYXEN_CMAKE%;%NYXEN_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build build --parallel 2
if errorlevel 1 exit /b 1
ctest --test-dir build --output-on-failure
exit /b %errorlevel%
