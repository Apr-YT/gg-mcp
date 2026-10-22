@echo off
rem build_ui.bat - build the PC-side search UI (imgui + Win32 + D3D11)
rem
rem Output: build\pc\gg-ui.exe  (statically linked, only system DLLs at runtime)
rem
rem imgui is vendored under third_party\imgui (v1.91.9, trimmed to what we compile).
rem If it is missing:
rem   git clone --depth 1 --branch v1.91.9 https://gitclone.com/github.com/ocornut/imgui.git third_party\imgui
rem (the mirror works from CN; github.com direct clone often resets)
setlocal
set ROOT=%~dp0..
set OUT=%ROOT%\build\pc
if not exist "%OUT%" mkdir "%OUT%"

if not exist "%ROOT%\third_party\imgui\imgui.cpp" (
  echo [FAIL] third_party\imgui missing - see the clone line at the top of this script
  exit /b 1
)

rem Fonts are NOT committed (msyh.ttc is 19MB). Copy once from the system font dir.
rem The UI also falls back to C:\Windows\Fonts at runtime, but a local copy is more
rem reliable (reading the .ttc straight from Fonts failed on one test machine).
if not exist "%ROOT%\build\msyh.ttc" (
  if exist "C:\Windows\Fonts\msyh.ttc" (
    copy /y "C:\Windows\Fonts\msyh.ttc" "%ROOT%\build\msyh.ttc" >nul
    echo [font] copied msyh.ttc
  )
)

g++ -std=c++17 -O2 -w -static -static-libgcc -static-libstdc++ ^
  -I "%ROOT%\third_party" -I "%ROOT%\shared" -I "%ROOT%\server" ^
  -I "%ROOT%\third_party\imgui" -I "%ROOT%\third_party\imgui\backends" ^
  "%ROOT%\tools\gg-ui\main.cpp" ^
  "%ROOT%\server\session.cpp" ^
  "%ROOT%\server\annotations.cpp" ^
  "%ROOT%\server\device\wire.cpp" ^
  "%ROOT%\third_party\imgui\imgui.cpp" ^
  "%ROOT%\third_party\imgui\imgui_draw.cpp" ^
  "%ROOT%\third_party\imgui\imgui_tables.cpp" ^
  "%ROOT%\third_party\imgui\imgui_widgets.cpp" ^
  "%ROOT%\third_party\imgui\backends\imgui_impl_win32.cpp" ^
  "%ROOT%\third_party\imgui\backends\imgui_impl_dx11.cpp" ^
  -o "%OUT%\gg-ui.exe" ^
  -lws2_32 -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lgdi32

if errorlevel 1 (
  echo [FAIL] gg-ui build failed
  exit /b 1
)
echo [OK] %OUT%\gg-ui.exe
endlocal
