@echo off
rem build_all.bat - Unified build script
rem Usage: build_all.bat [-Target pc|android|all]

set ROOT=%~dp0..

echo ========================================
echo   GG-MCP Unified Build
echo ========================================
echo.

echo [1/3] Building PC server...
call "%ROOT%\scripts\build_pc.bat"
if errorlevel 1 (
    echo [FAIL] PC build failed
    exit /b 1
)
echo [OK] PC server built

echo.
echo [2/3] Building Android agentd...
call "%ROOT%\scripts\build_ndk.bat"
if errorlevel 1 (
    echo [FAIL] Android build failed
    exit /b 1
)
echo [OK] Android agentd built

echo.
echo [3/3] Build summary...
if exist "%ROOT%\build\pc\gg-mcp-server.exe" (
    echo   PC:     gg-mcp-server.exe OK
) else (
    echo   PC:     MISSING
)
if exist "%ROOT%\build\agentd\gg-agentd" (
    echo   Android: gg-agentd OK
) else (
    echo   Android: MISSING
)

echo.
echo ========================================
echo   Build Complete
echo ========================================
