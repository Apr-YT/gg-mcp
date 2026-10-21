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
rem 2026-10-21 fix: this script used to end with an unconditional "exit /b 0",
rem so a cppcheck FAILURE was printed and then swallowed -- CI stayed green.
rem Now cppcheck runs with --error-exitcode=2 and a non-zero result fails the run.
rem All diagnostics located inside third_party are suppressed: json.hpp and
rem httplib.h are vendored single-header libs, their warnings are not ours to fix
rem (and json.hpp trips cppcheck's preprocessor on __has_feature / __has_warning).
where cppcheck >nul 2>nul
if errorlevel 1 goto :lint_nocppcheck
cppcheck --enable=warning,performance --error-exitcode=2 -q ^
  --suppress=*:*third_party* ^
  -I "%ROOT%\third_party" -I "%ROOT%\shared" ^
  "%ROOT%\agentd" "%ROOT%\server" "%ROOT%\native"
if errorlevel 2 goto :lint_cppcheck_fail
echo   cppcheck clean
goto :lint_cppcheck_done

:lint_nocppcheck
echo   [WARN] cppcheck not installed - static analysis SKIPPED (not a pass)
goto :lint_cppcheck_done

:lint_cppcheck_fail
rem NOTE: goto instead of a nested if/else block. Inside a parenthesised block
rem cmd silently truncates on unbalanced ')' and the "exit /b 1" below was
rem being skipped -- the script printed FAIL and still returned 0.
echo   [FAIL] cppcheck reported defects (see above)
endlocal
exit /b 1

:lint_cppcheck_done
echo.

echo === lint done ===
endlocal
exit /b 0

:check
rem %1 = file
findstr /R /C:" $" "%~1" >nul 2>nul && (echo   trailing-space: %~1 & set /a FOUND+=1)
findstr /C:"	" "%~1" >nul 2>nul && (echo   tab-char:       %~1 & set /a FOUND+=1)
exit /b 0