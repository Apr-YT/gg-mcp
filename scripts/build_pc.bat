@echo off
rem build_pc.bat - build PC-side gg-ghost-mcp with MinGW g++
setlocal
set ROOT=%~dp0..
set OUT=%ROOT%\build\pc
if not exist "%OUT%" mkdir "%OUT%"

g++ -std=c++17 -O3 -flto -march=native -Wall -Wextra ^
  -I "%ROOT%\third_party" -I "%ROOT%\shared" -I "%ROOT%\server" ^
  "%ROOT%\server\main.cpp" ^
  "%ROOT%\server\mcp\protocol.cpp" ^
  "%ROOT%\server\mcp\schema.cpp" ^
  "%ROOT%\server\session.cpp" ^
  "%ROOT%\server\annotations.cpp" ^
  "%ROOT%\server\device\wire.cpp" ^
  -o "%OUT%\gg-ghost-mcp.exe" -lws2_32

if errorlevel 1 (
  echo [FAIL] PC build failed
  exit /b 1
)
echo [OK] %OUT%\gg-ghost-mcp.exe
endlocal
