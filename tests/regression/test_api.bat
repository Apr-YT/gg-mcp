@echo off
rem test_api.bat - API regression test (HTTP transport)
rem NOTE: ASCII-only on purpose (cmd parses .bat in the ANSI codepage).
rem
rem 2026-10-15 fix: this script used build\pc\gg-mcp-server.exe (renamed to
rem gg-ghost-mcp.exe in commit b953bc5) and configs\gg-gghost.json (typo -- the
rem real file is configs\gg-ghost.json), so the server never started. It also
rem used cwd-relative paths, so it only worked when run from the repo root.
rem Paths are now rooted at %ROOT% and the health probe hits /healthz.
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\pc\gg-ghost-mcp.exe"
set "CFG=%ROOT%\configs\gg-ghost.json"

echo === GG-MCP API regression test ===
echo.

if not exist "%EXE%" (
    echo [FAIL] %EXE% not found - run scripts\build_pc.bat first
    exit /b 1
)
if not exist "%CFG%" (
    echo [FAIL] %CFG% not found
    exit /b 1
)

echo [1/3] starting server...
start "gg-ghost-mcp" /B "%EXE%" --config "%CFG%" --transport http --port 8788
REM 2026-10-21 fix: timeout.exe aborts with "Input redirection is not supported" when
REM there is no console (start /B + redirected streams), so the intended 3s wait never
REM happened -- the health check was racing the server start. ping -n is the only
REM stdin-independent delay available here.
ping -n 4 127.0.0.1 >nul

echo.
echo [2/3] HTTP health check...
curl -s -f http://127.0.0.1:8788/healthz >nul
if errorlevel 1 (
    echo [FAIL] HTTP server not responding
    taskkill /F /IM gg-ghost-mcp.exe >nul 2>nul
    exit /b 1
)
echo [OK] HTTP server responding

echo.
echo [3/3] cleanup...
taskkill /F /IM gg-ghost-mcp.exe >nul 2>nul
echo [OK]

echo.
echo === regression test done ===
endlocal
exit /b 0
