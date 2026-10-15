@echo off
rem test_all.bat - run the full local test suite (unit + integration + benchmark)
rem NOTE: ASCII-only on purpose. cmd parses .bat in the ANSI codepage (GBK here);
rem       UTF-8 Chinese in echo/comments shows as mojibake and can break parsing.
rem
rem 2026-10-15 fix: the previous version did `cd tests\unit` and then scanned for
rem test_*.exe in that directory. The unit binaries are produced into build\pc\,
rem so the for-loop body never executed -- the script printed [OK] without running
rem a single test. It also chained `cd tests\integration` relatively, so the later
rem stages ran in a directory that does not exist. Every stage is now addressed by
rem an absolute %ROOT% path and any failure aborts the run.
setlocal
set "ROOT=%~dp0.."

echo === GG-MCP test suite ===
echo.

echo [1/3] unit tests (build\pc\test_*.exe)...
if not exist "%ROOT%\build\pc" (
    echo [FAIL] %ROOT%\build\pc not found.
    echo        Build the unit tests first: tests\unit\run_tests.bat
    exit /b 1
)
set "RAN="
for %%t in ("%ROOT%\build\pc\test_*.exe") do (
    echo   --- %%~nxt ---
    "%%~ft"
    if errorlevel 1 ( echo [FAIL] %%~nxt & exit /b 1 )
    set "RAN=1"
)
if not defined RAN ( echo [FAIL] no unit test binaries found in build\pc & exit /b 1 )
echo [OK] unit tests
echo.

echo [2/3] integration tests (needs NDK clang + a rooted device on adb)...
set "NDK_CLANG=D:\Kiln\android-ndk-r27d-windows\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android28-clang.cmd"
if not exist "%NDK_CLANG%" goto :itest_nondk
set "HAS_DEV="
for /f "skip=1 tokens=1,2" %%a in ('adb devices 2^>nul') do if "%%b"=="device" set "HAS_DEV=1"
if not defined HAS_DEV goto :itest_nodev
powershell -ExecutionPolicy Bypass -File "%ROOT%\tests\integration\run_itest.ps1"
if errorlevel 1 ( echo [FAIL] integration tests & exit /b 1 )
goto :itest_done
:itest_nondk
echo [WARN] NDK clang not found - integration stage SKIPPED
goto :itest_done
:itest_nodev
echo [WARN] no adb device attached - integration stage SKIPPED
:itest_done
echo.

echo [3/3] benchmark...
if not exist "%ROOT%\tests\benchmark\memory_read.c" (
    echo [WARN] no benchmark source at tests\benchmark\memory_read.c - stage SKIPPED
    goto :bench_done
)
call "%ROOT%\scripts\benchmark.bat"
if errorlevel 1 ( echo [FAIL] benchmark & exit /b 1 )
:bench_done
echo.

echo === test suite done ===
endlocal
exit /b 0
