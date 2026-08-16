@echo off
setlocal enableextensions enabledelayedexpansion
set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

REM ===== parse args =====
set "CLEAN=0"
set "NO_CONFIG=0"
:parse_args
if "%~1"=="" goto :done_parse
if /i "%~1"=="--clean"     (set "CLEAN=1" & shift & goto :parse_args)
if /i "%~1"=="--no-config" (set "NO_CONFIG=1" & shift & goto :parse_args)
if /i "%~1"=="-h"          goto :show_help
if /i "%~1"=="--help"      goto :show_help
echo [warn] ignored unknown arg: %~1
shift
goto :parse_args
:done_parse

REM ===== only Release flavor remains (build\ + release\) =====
set "BUILD_DIR=build"
set "BUILD_TYPE=Release"
set "FLAVOR=RELEASE (windowed GUI subsystem)"

echo ============================================================
echo  openDock build: %FLAVOR%
echo    dir              = %BUILD_DIR%\
echo    CMAKE_BUILD_TYPE = %BUILD_TYPE%
echo ============================================================

REM ===== locate Visual Studio via vswhere (version/drive agnostic) =====
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERR] vswhere.exe not found - install Visual Studio 2022 or later
  goto :fail
)
set "VSINSTALLDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
  set "VSINSTALLDIR=%%i"
)
if "%VSINSTALLDIR%"=="" (
  echo [ERR] no Visual Studio with MSVC toolset detected
  goto :fail
)
echo [OK] VS = %VSINSTALLDIR%

REM ===== derive MSVC + Windows SDK env via Python (robust) =====
REM This machine's VsDevCmd.bat is broken (cmake / ConnectionManagerExe
REM extensions fail, vcvars dir lookup missing backslash). So we DO NOT
REM call vcvarsall. Instead a small Python helper (tools/msvc_env.py)
REM computes the newest MSVC toolchain + newest Windows SDK from disk and
REM writes a set-env batch file, which we call. This avoids all the fragile
REM batch string parsing around drive letters / parentheses / backslashes
REM that breaks on localized Windows.
set "PYTHON="
for %%p in (python py) do ( if not defined PYTHON ( %%p --version >nul 2>&1 && set "PYTHON=%%p" ) )
if not defined PYTHON ( if exist "C:\Python314\python.exe" set "PYTHON=C:\Python314\python.exe" )
if not defined PYTHON (
  echo [ERR] Python 3 not found on PATH - required to derive MSVC env
  echo       install Python 3 or add it to PATH, then retry
  goto :fail
)
set "GEN=%TEMP%\msvc_env_gen.bat"
if exist "%GEN%" del /f /q "%GEN%"
REM msvc_env.py third arg = debug flag (always 0 now; crtdbg.h check skipped)
"%PYTHON%" "%SCRIPT_DIR%\tools\msvc_env.py" "%VSINSTALLDIR%" "%GEN%" 0
if errorlevel 1 (
  echo [ERR] tools/msvc_env.py failed - see output above
  goto :fail
)
call "%GEN%"
if not defined MSVCDIR (
  echo [ERR] env file did not set MSVCDIR - aborting
  goto :fail
)

echo [STEP] MSVC %MSVCDIR%  /  Windows SDK %SDKVER%

echo ----- toolchain check -----
where cl    >nul 2>&1 && echo   [OK]   cl    || echo   [MISS] cl
where ninja >nul 2>&1 && echo   [OK]   ninja || echo   [MISS] ninja
where cmake >nul 2>&1 && echo   [OK]   cmake || echo   [MISS] cmake
echo ----------------------------

REM ===== optional pristine clean =====
if "%CLEAN%"=="1" (
  echo [STEP] clean: remove %BUILD_DIR%\
  if exist "%SCRIPT_DIR%\%BUILD_DIR%" rmdir /s /q "%SCRIPT_DIR%\%BUILD_DIR%"
  set "NO_CONFIG=0"
)
if not exist "%SCRIPT_DIR%\%BUILD_DIR%\CMakeCache.txt" set "NO_CONFIG=0"

REM ===== configure =====
if "%NO_CONFIG%"=="0" (
  echo [STEP] configure %BUILD_DIR% ...
  cmake -S "%SCRIPT_DIR%" -B "%SCRIPT_DIR%\%BUILD_DIR%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_CXX_COMPILER=cl.exe -DCMAKE_C_COMPILER=cl.exe
  if errorlevel 1 goto :fail
) else (
  echo [STEP] skip configure --no-config
)

REM ===== build =====
echo [STEP] build %BUILD_DIR% ...
cmake --build "%SCRIPT_DIR%\%BUILD_DIR%"
if errorlevel 1 goto :fail

REM ===== self-check: read back actual build type from CMakeCache =====
set "ACTUAL_TYPE="
for /f "tokens=1,* delims==" %%a in ('findstr /C:"CMAKE_BUILD_TYPE:STRING=" "%SCRIPT_DIR%\%BUILD_DIR%\CMakeCache.txt"') do set "ACTUAL_TYPE=%%b"

echo.
echo ============================================================
echo  build done: %BUILD_DIR%\openDock.exe
echo    actual CMAKE_BUILD_TYPE = %ACTUAL_TYPE%
if not "%ACTUAL_TYPE%"=="%BUILD_TYPE%" (
  echo    [X] build type mismatch! expected %BUILD_TYPE%, got %ACTUAL_TYPE%
  echo        CMakeCache may be polluted - run: build.bat --clean
  echo ============================================================
  goto :fail
)
echo    [OK] this is the RELEASE build - shippable ^(GUI window, no console^)

REM ===== package release artifacts into release\ =====
REM 将「可直接运行」的完整分发包打包到 release\：exe + 配置 + res 资源 + MSVC CRT/UCRT
REM 运行库（x64，/MD 动态链接必需，保证无 VS 的干净机器也能运行）。
REM VCToolsInstallDir / WindowsSdkDir 由 msvc_env.py 经 %GEN% 注入，此处沿用。
echo [STEP] package release -^> release\
set "REL=%SCRIPT_DIR%\release"
if exist "!REL!" rmdir /s /q "!REL!"
mkdir "!REL!"
copy /y "%SCRIPT_DIR%\%BUILD_DIR%\openDock.exe" "!REL!\"
if exist "%SCRIPT_DIR%\%BUILD_DIR%\config.json" copy /y "%SCRIPT_DIR%\%BUILD_DIR%\config.json" "!REL!\"
if exist "%SCRIPT_DIR%\%BUILD_DIR%\res" xcopy /e /i /y "%SCRIPT_DIR%\%BUILD_DIR%\res" "!REL!\res"
REM MSVC CRT (vcruntime140 / vcruntime140_1 / msvcp140) + UCRT (ucrtbase + api-ms-win-crt-*)
REM UCRT redist 位于版本化子目录 Redist\<WindowsSDKVersion>\ucrt\DLLs\x64
set "VCBIN=%VCToolsInstallDir%bin\Hostx64\x64"
set "UCRTDIR=%WindowsSdkDir%Redist\%WindowsSDKVersion%ucrt\DLLs\x64"
for %%f in (vcruntime140.dll vcruntime140_1.dll msvcp140.dll) do (
  if exist "!VCBIN!\%%f" copy /y "!VCBIN!\%%f" "!REL!\"
)
if exist "!UCRTDIR!" xcopy /e /i /y "!UCRTDIR!\*.dll" "!REL!\"
echo    [OK] release packaged: !REL!\  (openDock.exe + config.json + res\ + CRT/UCRT dlls)

echo ============================================================
goto :done

:show_help
echo Usage:
echo   build.bat               RELEASE -^> build\     (Release, GUI)
echo                           然后自动打包 release\ (exe + config + res + CRT/UCRT dlls)
echo   build.bat --clean       remove build dir first, then clean build
echo   build.bat --no-config   incremental build only ^(skip configure^)
goto :done

:fail
echo ============================================================
echo FAILED - check the [MISS] / [ERR] lines above
echo ============================================================
pause
exit /b 1

:done
