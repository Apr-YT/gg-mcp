@echo off
rem test_api.bat - API回归测试

echo === GG-MCP API回归测试 ===
echo.

echo 测试1: 启动服务
start /B build\pc\gg-mcp-server.exe --config configs\gg-gghost.json
timeout /t 3 /nobreak >nul

echo.
echo 测试2: HTTP健康检查
curl -s http://127.0.0.1:8788/tools/list
if errorlevel 1 (
    echo [FAIL] HTTP服务未响应
    taskkill /F /IM gg-mcp-server.exe 2>nul
    exit /b 1
)
echo [OK] HTTP服务正常

echo.
echo 测试3: 清理
taskkill /F /IM gg-mcp-server.exe 2>nul
echo [OK]

echo.
echo === 回归测试完成 ===
