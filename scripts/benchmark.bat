@echo off
rem benchmark.bat - performance benchmark (PC-side search/memory throughput)
rem NOTE: ASCII-only on purpose (cmd parses .bat in the ANSI codepage).
rem
rem 2026-10-15: this script built tests\benchmark\memory_read.c, which does not
rem exist anywhere in the repo (verified: `find tests -type f` lists no benchmark
rem sources), so it always died at the compile step with a confusing "No such
rem file or directory" from the compiler. It now states the real situation.
rem To enable this stage, add a source at tests\benchmark\memory_read.c.
setlocal
set "ROOT=%~dp0.."
set "BENCHMARK=%ROOT%\tests\benchmark"
set "SRC=%BENCHMARK%\memory_read.c"

echo === GG-MCP benchmark ===
echo.

if not exist "%SRC%" goto :no_src

echo [1/3] Building benchmark...
g++ -O2 -Wall -o "%BENCHMARK%\memory_read.exe" "%SRC%"
if errorlevel 1 ( echo [FAIL] build failed & endlocal & exit /b 1 )
echo [OK]
echo.

echo [2/3] Running benchmark...
"%BENCHMARK%\memory_read.exe"
echo.

echo [3/3] Saving results...
"%BENCHMARK%\memory_read.exe" > "%ROOT%\benchmark_results.txt"
echo [OK] results written to benchmark_results.txt
echo.
echo === benchmark done ===
endlocal
exit /b 0

:no_src
echo [FAIL] benchmark source not found: tests\benchmark\memory_read.c
echo        No benchmark source exists in this repo, so this stage cannot run.
echo        Add one at that path to enable it, or drop the stage from your workflow.
endlocal
exit /b 1
