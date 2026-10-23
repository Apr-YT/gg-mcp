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
start "GG-MCP-Server" "build\pc\gg-ghost-mcp.exe" --config "configs\gg-ghost.json" --transport http --port 8788 %*
