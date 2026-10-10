@echo off
rem Builds AudioBridge with MSVC and deploys the Qt runtime next to the executable.
rem Usage: Tools\Build.cmd [QtKitPath]   (default: C:\Development\Qt\6.9.1\msvc2022_64)
rem Output goes to ..\Build\Windows beside this checkout.
setlocal
set "QTKIT=%~1"
if "%QTKIT%"=="" set "QTKIT=C:\Development\Qt\6.9.1\msvc2022_64"
set "VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Professional"
set "SOURCE=%~dp0.."
set "BUILD=%~dp0..\..\Build\Windows"

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

cmake -S "%SOURCE%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QTKIT%" || exit /b 1
cmake --build "%BUILD%" || exit /b 1
"%QTKIT%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "%BUILD%\AudioBridge.exe" || exit /b 1
