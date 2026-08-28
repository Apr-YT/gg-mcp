@echo off
rem test_compile.bat - 测试编译优化版本

setlocal
set ROOT=%~dp0..
set NDK=D:\Kiln\android-ndk-r27d-windows
set CAPSTONE=D:\Kiln\capstone\capstone-5.0.1
set BIN=%NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin
set CC=%BIN%\aarch64-linux-android28-clang.cmd

echo Testing compilation with optimization modules...
echo.

call "%CC%" -O3 -flto -Wall -I"%ROOT%" -I"%CAPSTONE%\include" -o "%ROOT%\build\agentd\gg-agentd-test" ^
  "%ROOT%\agentd\main.c" ^
  "%ROOT%\agentd\process.c" ^
  "%ROOT%\agentd\search.c" ^
  "%ROOT%\agentd\freeze.c" ^
  "%ROOT%\agentd\watch.c" ^
  "%ROOT%\agentd\disasm.c" ^
  "%ROOT%\agentd\elfsym.c" ^
  "%ROOT%\agentd\loader.c" ^
  "%ROOT%\agentd\version.c" ^
  "%ROOT%\agentd\perf_simple.c" ^
  "%ROOT%\third_party\cJSON.c" ^
  "%CAPSTONE%\libcapstone.a" -pie

if errorlevel 1 (
    echo [FAIL] Compilation failed
    exit /b 1
)

echo [OK] Compilation successful
echo Output: %ROOT%\build\agentd\gg-agentd-test
