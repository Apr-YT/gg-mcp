@echo off
set ROOT=%~dp0..
set SRCS=%ROOT%\server\proxy-server.cpp
set OUT=%ROOT%\build\pc\proxy-server.exe

echo === 编译HTTP代理服务 ===
echo.

if not exist "%ROOT%\build\pc" mkdir "%ROOT%\build\pc"

echo 编译中...
g++ -std=c++17 -O3 -Wall -I "%ROOT%\third_party" "%SRCS%" -o "%OUT%" -lws2_32

if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)

echo [OK] 编译完成
echo 产物: %OUT%