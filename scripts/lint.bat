@echo off
rem lint.bat - 代码质量检查

set ROOT=%~dp0..

echo === GG-MCP 代码质量检查 ===
echo.

echo [1/4] 检查代码风格...
echo 检查C代码风格...

for %%f in (agentd\*.c) do (
    findstr /C:"\t" "%%f" >nul && echo   Tab字符: %%f
    findstr " $" "%%f" >nul && echo   行尾空格: %%f
)

echo [OK]

echo.
echo [2/4] 检查TODO注释...
findstr /S /I "TODO FIXME" agentd server native
echo [OK]

echo.
echo [3/4] 代码统计...
for %%d in (agentd server) do (
    echo %%d:
    find /c /v "" %%d\*.c %%d\*.h
)

echo [OK]

echo.
echo [4/4] 函数复杂度分析...
findstr /S /C:"{" agentd\*.c | find /V "}" agentd\*.c
echo [OK]

echo.
echo === 代码质量检查完成 ===
