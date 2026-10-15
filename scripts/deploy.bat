@echo off
rem deploy.bat - 自动部署

set AGENTD=build\agentd\gg-agentd

echo [1/4] 检查agentd...
if not exist "%AGENTD%" (
    echo [FAIL] agentd not found
    exit /b 1
)
echo [OK]

echo.
echo [2/4] 获取设备...
for /f "tokens=1" %%d in ('adb devices ^| findstr "device"') do set DEVICE=%%d
if "%DEVICE%"=="" (
    echo [FAIL] No device
    exit /b 1
)
echo [OK] Device: %DEVICE%

echo.
echo [3/4] 部署...
adb -s %DEVICE shell su -c "killall gg-agentd" 2>nul
adb push "%AGENTD%" /data/local/tmp/ggmcp/gg-agentd
adb shell su -c "chmod 755 /data/local/tmp/ggmcp/gg-agentd"
adb shell su -c "cd /data/local/tmp/ggmcp && ./gg-agentd --driver rwproc --port 34127 &"

echo.
echo [4/4] 验证...
rem NOTE: fully-qualified - a bare `timeout` is shadowed by Git-Bash/MSYS coreutils.
"%SystemRoot%\System32\timeout.exe" /t 3 /nobreak >nul
adb shell ps -A | findstr gg-agentd
echo.
echo === 完成 ===
