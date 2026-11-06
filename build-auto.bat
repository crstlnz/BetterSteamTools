@echo off
setlocal EnableDelayedExpansion

REM ---------------------------------------------------------------------------
REM build-auto.bat - one-click build that sets up the MSVC x64 environment first.
REM
REM build.bat alone only works from a "Native Tools" / "Developer" command
REM prompt, because it needs cl.exe, link.exe AND rc.exe/mt.exe from the Windows
REM SDK on PATH. Run from a plain cmd/PowerShell/Explorer window, the CMake
REM compiler check fails with "rc ... no such file or directory".
REM
REM This wrapper locates Visual Studio with vswhere, imports the x64 toolchain
REM environment via vcvars64.bat, and then hands over to build.bat. All build
REM options of build.bat still apply:
REM   GENERATOR, ARCH, CONFIGS  (e.g. SET CONFIGS=Release && build-auto.bat)
REM   OPENSTEAMTOOL_VERSION     (e.g. SET OPENSTEAMTOOL_VERSION=v1.2.3)
REM
REM Extra options of this wrapper:
REM   --clean      delete the build/ directory before configuring
REM   --regen      delete build/CMakeCache.txt only (keeps fetched deps' objs)
REM ---------------------------------------------------------------------------

cd /d "%~dp0"

set "CLEAN=0"
set "REGEN=0"
for %%A in (%*) do (
    if /I "%%~A"=="--clean" set "CLEAN=1"
    if /I "%%~A"=="--regen" set "REGEN=1"
    if /I "%%~A"=="--help"  goto :usage
    if /I "%%~A"=="-h"      goto :usage
)

REM Already inside a developer environment? Then vcvars is unnecessary.
where cl.exe >nul 2>nul && where rc.exe >nul 2>nul
if not errorlevel 1 (
    echo [INFO] MSVC environment already initialized.
    goto :build
)

REM ---------------------------------------------------------------------------
REM Locate Visual Studio 2022+ with the x64 C++ toolset.
REM ---------------------------------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found. Install Visual Studio with the
    echo         "Desktop development with C++" workload.
    goto :fail
)

set "VSPATH="
for /f "usebackq delims=" %%P in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%P"

if "%VSPATH%"=="" (
    echo [ERROR] No Visual Studio installation with the x64 C++ toolset was found.
    goto :fail
)
echo [INFO] Visual Studio: %VSPATH%

set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [ERROR] vcvars64.bat not found at:
    echo         %VCVARS%
    goto :fail
)

REM vcvars64.bat also puts the Windows SDK's rc.exe/mt.exe on PATH. If the SDK
REM itself is missing, fail early with a readable message instead of a cryptic
REM CMake "compiler is broken" error.
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] Failed to initialize the x64 toolchain via vcvars64.bat.
    goto :fail
)

set "MISSING="
where cl.exe  >nul 2>nul || set "MISSING=!MISSING! cl.exe"
where rc.exe  >nul 2>nul || set "MISSING=!MISSING! rc.exe"
where mt.exe  >nul 2>nul || set "MISSING=!MISSING! mt.exe"
if not "!MISSING!"=="" (
    echo [ERROR] Still missing after vcvars64:!MISSING!
    echo         Install the "Windows 10/11 SDK" component in the Visual Studio
    echo         Installer ^(Individual components^) and retry.
    goto :fail
)
echo [INFO] Toolchain ready: cl.exe, rc.exe, mt.exe

:build
REM ---------------------------------------------------------------------------
REM Optional cache handling. CMake refuses to reuse a build/ cache created with
REM a different generator, and a stale cache is a common local-build failure.
REM ---------------------------------------------------------------------------
if "%CLEAN%"=="1" (
    echo [INFO] Removing build directory...
    if exist build rmdir /s /q build
) else if "%REGEN%"=="1" (
    echo [INFO] Removing build\CMakeCache.txt...
    if exist "build\CMakeCache.txt" del /q "build\CMakeCache.txt"
)

if not "%OPENSTEAMTOOL_VERSION%"=="" echo [INFO] Version: %OPENSTEAMTOOL_VERSION%

call "%~dp0build.bat"
if errorlevel 1 goto :fail

exit /b 0

:usage
echo Usage: build-auto.bat [--clean] [--regen]
echo.
echo   --clean   delete build/ before configuring
echo   --regen   delete build/CMakeCache.txt before configuring
echo.
echo Environment:
echo   GENERATOR             CMake generator (default: Ninja Multi-Config)
echo   ARCH                  architecture for VS generators (default: x64)
echo   CONFIGS               configurations to build (default: Release Debug)
echo   OPENSTEAMTOOL_VERSION version string embedded into the DLL
exit /b 0

:fail
echo [ERROR] Build failed.
exit /b 1
