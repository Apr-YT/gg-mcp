@echo off
rem start_server.bat - launch the PC-side MCP server (http transport) from repo root.
rem
rem Same reason as start_ui.bat: config is a relative path, so CWD must be the
rem repo root. Started this way the process is owned by explorer, not by an
rem agent shell - it keeps running after the launching session ends.
rem
rem usage: double-click, or scripts\start_server.bat [--port 8788]
cd /d "%~dp0.."
echo [start_server] cwd = %CD%
if not exist "build\pc\gg-ghost-mcp.exe" (
    echo [start_server] ERROR: build\pc\gg-ghost-mcp.exe not found. Run scripts\build_pc.bat first.
    pause
    exit /b 1
)
if not exist "configs\gg-ghost.json" (
    echo [start_server] ERROR: configs\gg-ghost.json not found. Run deploy first.
    pause
    exit /b 1
)
rem Exit code passthrough: a started process exits immediately when the exe
rem rejects config or port - keep the window open so the reason is readable.
start "GG-MCP-Server" cmd /c ""build\pc\gg-ghost-mcp.exe" --config "configs\gg-ghost.json" --transport http --port 8788 %* & if errorlevel 1 pause"
