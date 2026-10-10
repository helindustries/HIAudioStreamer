@echo off
rem Builds HIAudioStreamer with MSVC, stages it with the Qt and C++ runtimes, and packages the
rem Inno Setup installer into the build directory.
rem Usage: Tools\Build.cmd [BuildDir]   (default: ..\Build\Windows beside this checkout)
rem   QT_ROOT_DIR  the MSVC Qt kit (default: C:\Development\Qt\6.9.1\msvc2022_64)
rem   ISCC         the Inno Setup compiler (default: Inno Setup 6 under Program Files (x86))
setlocal
set "SOURCE=%~dp0.."
set "BUILD=%~1"
if "%BUILD%"=="" set "BUILD=%~dp0..\..\Build\Windows"
if "%QT_ROOT_DIR%"=="" set "QT_ROOT_DIR=C:\Development\Qt\6.9.1\msvc2022_64"
if "%ISCC%"=="" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

rem vswhere's -requires misses instances it considers incomplete, so the instance is chosen by
rem what is on disk: any with the x64 tools, preferably one that also bundles CMake and Ninja.
set "VSCMAKE=Common7\IDE\CommonExtensions\Microsoft\CMake"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -property installationPath`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" if not defined VSROOT set "VSROOT=%%i"
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" if exist "%%i\%VSCMAKE%\CMake\bin\cmake.exe" set "VSROOT=%%i"
)
if "%VSROOT%"=="" (
    echo %~nx0: no Visual Studio instance listed by "%VSWHERE%" has vcvars64.bat 1>&2
    exit /b 2
)
if not exist "%ISCC%" (
    echo %~nx0: Inno Setup compiler not found at "%ISCC%"; set ISCC to its path 1>&2
    exit /b 3
)

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSROOT%\%VSCMAKE%\CMake\bin;%VSROOT%\%VSCMAKE%\Ninja;%PATH%"

cmake -S "%SOURCE%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_ROOT_DIR%" || exit /b 1
cmake --build "%BUILD%" || exit /b 1
if exist "%BUILD%\Deploy" rmdir /s /q "%BUILD%\Deploy"
cmake --install "%BUILD%" --prefix "%BUILD%\Deploy" || exit /b 1
"%QT_ROOT_DIR%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw --no-compiler-runtime --skip-plugin-types generic "%BUILD%\Deploy\HIAudioStreamer.exe" || exit /b 1

rem The C++ runtime goes in app-locally, which Microsoft permits, rather than as vc_redist. The
rem redist of the newest installed tools can be a stub while an older toolset is selected, so the
rem one matching the compiler is the fallback.
set "CRT=%VCToolsRedistDir%x64\Microsoft.VC143.CRT"
if not exist "%CRT%\vcruntime140.dll" set "CRT=%VCINSTALLDIR%Redist\MSVC\%VCToolsVersion%\x64\Microsoft.VC143.CRT"
if not exist "%CRT%\vcruntime140.dll" (
    echo %~nx0: no C++ runtime under "%VCToolsRedistDir%" or for toolset %VCToolsVersion% 1>&2
    exit /b 4
)
copy /y "%CRT%\*.dll" "%BUILD%\Deploy" >nul || exit /b 1
"%ISCC%" /Q "%BUILD%\HIAudioStreamer.iss" || exit /b 1
