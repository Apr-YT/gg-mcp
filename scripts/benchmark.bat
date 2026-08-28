@echo off
rem benchmark.bat - 性能基准测试

set ROOT=%~dp0..
set BENCHMARK=%ROOT%\tests\benchmark

echo === GG-MCP 性能基准测试 ===
echo.

echo [1/3] 编译基准测试...
g++ -O2 -Wall -o "%BENCHMARK%\memory_read.exe" "%BENCHMARK%\memory_read.c"
if errorlevel 1 (
    echo [FAIL] 编译失败
    exit /b 1
)
echo [OK]

echo.
echo [2/3] 运行基准测试...
"%BENCHMARK%\memory_read.exe"
echo.

echo [3/3] 保存结果...
"%BENCHMARK%\memory_read.exe" > "%ROOT%\benchmark_results.txt"
echo [OK] 结果已保存到 benchmark_results.txt

echo.
echo === 基准测试完成 ===
