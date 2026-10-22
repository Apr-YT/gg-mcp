@echo off
rem build_ndk.bat - build device artifacts with Android NDK (agentd only)
rem usage: build_ndk.bat [NDK dir] [Capstone dir]
rem   NDK default:      D:\Kiln\android-ndk-r30
rem   Capstone default: D:\Kiln\capstone\capstone-5.0.1
setlocal
set ROOT=%~dp0..
set NDK=%1
if "%NDK%"=="" set NDK=D:\Kiln\android-ndk-r30
set CAPSTONE=%2
if "%CAPSTONE%"=="" set CAPSTONE=D:\Kiln\capstone\capstone-5.0.1
set BIN=%NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin
set CC=%BIN%\aarch64-linux-android28-clang.cmd
set OUT=%ROOT%\build\native
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%ROOT%\build\agentd" mkdir "%ROOT%\build\agentd"

if not exist "%CC%" (
  echo [FAIL] NDK clang not found at %CC%
  exit /b 1
)
if not exist "%CAPSTONE%\libcapstone.a" (
  echo [FAIL] %CAPSTONE%\libcapstone.a not found.
  exit /b 1
)

echo [1/1] gg-agentd (with optimizations) ...
call "%CC%" -O3 -flto -Wall -I"%ROOT%" -I"%CAPSTONE%\include" -o "%ROOT%\build\agentd\gg-agentd" ^
  "%ROOT%\agentd\main.c" ^
  "%ROOT%\agentd\cmd_shell.c" ^
  "%ROOT%\agentd\cmd_view.c" ^
  "%ROOT%\agentd\cmd_search.c" ^
  "%ROOT%\agentd\cmd_mem.c" ^
  "%ROOT%\agentd\process.c" ^
  "%ROOT%\agentd\driver.c" ^
  "%ROOT%\agentd\search.c" ^
  "%ROOT%\agentd\freeze.c" ^
  "%ROOT%\agentd\watch.c" ^
  "%ROOT%\agentd\disasm.c" ^
  "%ROOT%\agentd\elfsym.c" ^
  "%ROOT%\agentd\version.c" ^
  "%ROOT%\agentd\perf_simple.c" ^
  "%ROOT%\third_party\cJSON.c" ^
  "%CAPSTONE%\libcapstone.a" -pie
if errorlevel 1 ( echo [FAIL] agentd & exit /b 1 )

echo [OK] build\agentd\gg-agentd
endlocal
