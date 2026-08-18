@echo off
rem build_ndk.bat - build device artifacts with Android NDK (agentd + hook + stub)
rem usage: build_ndk.bat [NDK dir] [Capstone dir]
rem   NDK default:      D:\Kiln\android-ndk-r27d-windows
rem   Capstone default: D:\Kiln\capstone\capstone-5.0.1 (needs cross-built libcapstone.a)
setlocal
set ROOT=%~dp0..
set NDK=%1
if "%NDK%"=="" set NDK=D:\Kiln\android-ndk-r27d-windows
set CAPSTONE=%2
if "%CAPSTONE%"=="" set CAPSTONE=D:\Kiln\capstone\capstone-5.0.1
set BIN=%NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin
set CC=%BIN%\aarch64-linux-android28-clang.cmd
set OUT=%ROOT%\build\native
if not exist "%OUT%" mkdir "%OUT%"

if not exist "%CC%" (
  echo [FAIL] NDK clang not found at %CC%
  exit /b 1
)
if not exist "%CAPSTONE%\libcapstone.a" (
  echo [FAIL] %CAPSTONE%\libcapstone.a not found.
  echo        Cross-build it first, e.g.:
  echo        cd /d %CAPSTONE%
  echo        mingw32-make all CAPSTONE_STATIC=yes CAPSTONE_SHARED=no CAPSTONE_BUILD_CORE_ONLY=yes CAPSTONE_ARCHS="arm aarch64" CC=%CC% AR=%BIN%\llvm-ar.exe RANLIB=%BIN%\llvm-ranlib.exe BUILDDIR=build-gg
  echo        then copy build-gg\libcapstone.a to %CAPSTONE%\libcapstone.a
  exit /b 1
)

echo [1/3] gg-agentd ...
call "%CC%" -O2 -Wall -I"%ROOT%" -I"%CAPSTONE%\include" -o "%ROOT%\build\agentd\gg-agentd" ^
  "%ROOT%\agentd\main.c" "%ROOT%\agentd\driver.c" "%ROOT%\agentd\process.c" ^
  "%ROOT%\agentd\search.c" "%ROOT%\agentd\freeze.c" "%ROOT%\agentd\version.c" ^
  "%ROOT%\agentd\loader.c" "%ROOT%\agentd\watch.c" "%ROOT%\agentd\disasm.c" ^
  "%ROOT%\agentd\elfsym.c" ^
  "%ROOT%\third_party\cJSON.c" "%CAPSTONE%\libcapstone.a" -pie
if errorlevel 1 ( echo [FAIL] agentd & exit /b 1 )

echo [2/3] libKernelGg.so (syscall 270 hook) ...
call "%CC%" -shared -fPIC -O2 -Wall -o "%OUT%\libKernelGg.so" "%ROOT%\native\hook\hook.c"
if errorlevel 1 ( echo [FAIL] hook & exit /b 1 )

echo [3/3] lib5_stub (static PIE ELF stub) ...
call "%CC%" -static-pie -fPIE -O2 -Wall -o "%OUT%\lib5_stub" "%ROOT%\native\stub\lib5_stub.c"
if errorlevel 1 ( echo [FAIL] stub & exit /b 1 )

echo [OK] build\native\libKernelGg.so  build\native\lib5_stub  build\agentd\gg-agentd
endlocal
