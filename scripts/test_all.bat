@echo off
rem test_all.bat - 运行所有测试

echo === GG-MCP 测试套件 ===
echo.

echo [1/3] 单元测试...
cd tests\unit
for %%t in (test_*.exe) do (
    echo 运行 %%t...
    "%%t.exe"
)
echo [OK]

echo.
echo [2/3] 集成测试...
cd tests\integration
powershell -ExecutionPolicy Bypass -File run_itest.ps1
echo [OK]

echo.
echo [3/3] 性能基准...
cd scripts
call benchmark.bat
echo [OK]

echo.
echo === 测试完成 ===
