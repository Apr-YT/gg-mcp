@echo off
rem build_all.bat - Unified build script
rem Usage: build_all.bat [-Target pc|android|all]   (default: all)
rem
rem 2026-10-15 fix: the summary used to check build\pc\gg-mcp-server.exe, but
rem build_pc.bat emits gg-ghost-mcp.exe (renamed in commit b953bc5 "命名统一"),
rem so "PC:" always reported MISSING. Also -Target was documented but ignored,
rem and the summary never influenced the exit code. Now: -Target is honoured,
rem the artifact names match the build scripts, and a missing artifact for a
rem requested target fails the build (so CI cannot pass on an empty output).
setlocal
set "ROOT=%~dp0.."

set "TARGET=all"
if /i "%~1"=="-Target" set "TARGET=%~2"
if "%TARGET%"=="" set "TARGET=all"

set "PC_BIN=%ROOT%\build\pc\gg-ghost-mcp.exe"
set "AGENTD_BIN=%ROOT%\build\agentd\gg-agentd"

echo ========================================
echo   GG-MCP Unified Build  [target=%TARGET%]
echo ========================================
echo.

if /i "%TARGET%"=="android" goto :skip_pc
echo [1/3] Building PC server...
call "%ROOT%\scripts\build_pc.bat"
if errorlevel 1 ( echo [FAIL] PC build failed & exit /b 1 )
echo [OK] PC server built
echo.
goto :after_pc
:skip_pc
echo [1/3] PC server skipped -Target android
echo.
:after_pc

if /i "%TARGET%"=="pc" goto :skip_android
echo [2/3] Building Android agentd...
call "%ROOT%\scripts\build_ndk.bat"
if errorlevel 1 ( echo [FAIL] Android build failed & exit /b 1 )
echo [OK] Android agentd built
echo.
goto :after_android
:skip_android
echo [2/3] Android agentd skipped -Target pc
echo.
:after_android

echo [3/3] Build summary...
set "MISSING=0"
if /i "%TARGET%"=="android" goto :sum_android
if exist "%PC_BIN%" (echo   PC:      build\pc\gg-ghost-mcp.exe  OK) else (echo   PC:      build\pc\gg-ghost-mcp.exe  MISSING & set "MISSING=1")
:sum_android
if /i "%TARGET%"=="pc" goto :sum_done
if exist "%AGENTD_BIN%" (echo   Android: build\agentd\gg-agentd  OK) else (echo   Android: build\agentd\gg-agentd  MISSING & set "MISSING=1")
:sum_done

echo.
if "%MISSING%"=="1" (
    echo [FAIL] a requested target produced no artifact
    endlocal
    exit /b 1
)

echo ========================================
echo   Build Complete
echo ========================================
endlocal
exit /b 0
