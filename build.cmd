@echo off
rem Builds cs2_gc with Visual Studio 2022 + vcpkg (x64, static CRT).
rem Only compiles. It does not start or touch CS2.
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio 2022 not found
    exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not defined VCPKG_ROOT set "VCPKG_ROOT=%VSDIR%\VC\vcpkg"

set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Release"

cmake -S "%~dp0." -B "%~dp0build" -G Ninja ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
    -DOUTDIR="%~dp0release" || exit /b 1

cmake --build "%~dp0build" --config %BUILD_TYPE% || exit /b 1

echo.
echo Done. Output is in "%~dp0release"
