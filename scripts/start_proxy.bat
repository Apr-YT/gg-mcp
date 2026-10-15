@echo off
set ROOT=%~dp0..
set PROXY=%ROOT%\build\pc\proxy-server.exe

echo === 启动GG-MCP Web UI ===
echo.

if not exist "%PROXY%" (
    echo [ERROR] 代理服务未找到
    echo 请先运行: .\build_proxy.bat
    pause
    exit /b 1
)

echo 启动HTTP代理服务...
start "GG-MCP Proxy" "%PROXY%"

rem NOTE: fully-qualified - a bare `timeout` is shadowed by Git-Bash/MSYS coreutils.
"%SystemRoot%\System32\timeout.exe" /t 2 /nobreak >nul

echo 打开浏览器...
start http://127.0.0.1:8789

echo.
echo === 启动完成 ===
echo Web UI: http://127.0.0.1:8789