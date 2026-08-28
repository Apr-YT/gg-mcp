@echo off
rem lint.bat - code quality checks (ASCII-only on purpose: cmd parses .bat in ANSI codepage)
setlocal
set ROOT=%~dp0..

echo === GG-MCP lint ===
echo.

echo [1/4] Trailing whitespace + tab check (agentd/server/native C sources)...
set FOUND=0
for /R "%ROOT%\agentd" %%f in (*.c *.h) do call :check "%%f"
for /R "%ROOT%\server" %%f in (*.cpp *.h) do call :check "%%f"
for /R "%ROOT%\native" %%f in (*.c *.h) do call :check "%%f"
if "%FOUND%"=="0" (echo   clean) else (echo   %FOUND% files flagged - fix or whitelist)
echo.

echo [2/4] TODO/FIXME markers...
findstr /S /I "TODO FIXME" "%ROOT%\agentd\*.c" "%ROOT%\agentd\*.h" "%ROOT%\server\*.cpp" "%ROOT%\server\*.h" "%ROOT%\native\*.c" 2>nul
echo   (list above, if any)
echo.

echo [3/4] Line counts...
find /c /v "" "%ROOT%\agentd\*.c" "%ROOT%\server\*.cpp" 2>nul | findstr /C:": " 
echo.

echo [4/4] cppcheck (optional, skipped if not installed)...
where cppcheck >nul 2>nul
if errorlevel 1 (
  echo   [WARN] cppcheck not installed. Install and run:
  echo     cppcheck --enable=warning,performance -I third_party -I shared --inline-suppr agentd server native
) else (
  cppcheck --enable=warning,performance -q -I "%ROOT%\third_party" -I "%ROOT%\shared" "%ROOT%\agentd" "%ROOT%\server" "%ROOT%\native"
  echo   cppcheck done (issues above, if any)
)
echo.

echo === lint done ===
endlocal
exit /b 0

:check
rem %1 = file
findstr /R /C:" $" "%~1" >nul 2>nul && (echo   trailing-space: %~1 & set /a FOUND+=1)
findstr /C:"	" "%~1" >nul 2>nul && (echo   tab-char:       %~1 & set /a FOUND+=1)
exit /b 0