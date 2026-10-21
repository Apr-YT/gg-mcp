@echo off
rem test_all.bat - run the full local test suite (unit + stdio smoke + integration + benchmark)
rem NOTE: ASCII-only on purpose. cmd parses .bat in the ANSI codepage (GBK here);
rem       UTF-8 Chinese in echo/comments shows as mojibake and can break parsing.
rem
rem 2026-10-15 fix: the previous version did `cd tests\unit` and then scanned for
rem test_*.exe in that directory. The unit binaries are produced into build\pc\,
rem so the for-loop body never executed -- the script printed [OK] without running
rem a single test. It also chained `cd tests\integration` relatively, so the later
rem stages ran in a directory that does not exist. Every stage is now addressed by
rem an absolute %ROOT% path and any failure aborts the run.
rem
rem 2026-10-21 changes:
rem   a) new stage [2/4]: PC stdio end-to-end smoke (tests\regression\test_stdio_smoke.py).
rem      Runs with no device attached; covers the JSON-RPC layer the unit tests skip.
rem   b) integration stage: NDK path was still r27d (does not exist on this machine),
rem      so the stage always skipped with a misleading "NDK clang not found" while the
rem      real blocker was "no device". Device is now probed FIRST, NDK path is r30.
setlocal
set "ROOT=%~dp0.."

echo === GG-MCP test suite ===
echo.

echo [1/4] unit tests (delegated to tests\unit\run_tests.bat)...
rem 2026-10-21 fix: this stage used to glob build\pc\test_*.exe and run whatever
rem it found. That included stale binaries whose source had been deleted
rem (test_ko_downgrade.exe survived after test_ko_downgrade.c was removed), so a
rem run could "pass" on code that no longer exists. run_tests.bat is the single
rem source of truth: it rebuilds every test it knows about, then runs them.
if not exist "%ROOT%\tests\unit\run_tests.bat" (
    echo [FAIL] tests\unit\run_tests.bat not found
    exit /b 1
)
call "%ROOT%\tests\unit\run_tests.bat"
if errorlevel 1 ( echo [FAIL] unit tests & exit /b 1 )
echo [OK] unit tests
echo.

echo [2/4] stdio end-to-end smoke (PC only, no device needed)...
set "PY="
if defined GG_PYTHON (
    "%GG_PYTHON%" -c "import sys;sys.exit(0 if sys.version_info[0]==3 else 1)" >nul 2>nul
    if not errorlevel 1 set "PY=%GG_PYTHON%"
)
if not defined PY (
    for /f "delims=" %%p in ('where python 2^>nul') do (
        if not defined PY (
            "%%p" -c "import sys;sys.exit(0 if sys.version_info[0]==3 else 1)" >nul 2>nul
            if not errorlevel 1 set "PY=%%p"
        )
    )
)
if not defined PY goto :smoke_nopy
"%PY%" "%ROOT%\tests\regression\test_stdio_smoke.py"
if errorlevel 1 ( echo [FAIL] stdio smoke & exit /b 1 )
goto :smoke_done
:smoke_nopy
echo [WARN] python 3 not found - stdio smoke SKIPPED ^(set GG_PYTHON=<full path> to run it^)
:smoke_done
echo.

echo [3/4] integration tests (needs a rooted device on adb + NDK clang)...
set "HAS_DEV="
for /f "skip=1 tokens=1,2" %%a in ('adb devices 2^>nul') do if "%%b"=="device" set "HAS_DEV=1"
if not defined HAS_DEV goto :itest_nodev
set "NDK_CLANG=D:\Kiln\android-ndk-r30\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android28-clang.cmd"
if not exist "%NDK_CLANG%" goto :itest_nondk
powershell -ExecutionPolicy Bypass -File "%ROOT%\tests\integration\run_itest.ps1"
if errorlevel 1 ( echo [FAIL] integration tests & exit /b 1 )
goto :itest_done
:itest_nondk
echo [WARN] NDK clang not found at %NDK_CLANG% - integration stage SKIPPED
goto :itest_done
:itest_nodev
echo [WARN] no adb device attached - integration stage SKIPPED
:itest_done
echo.

echo [4/4] benchmark...
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
