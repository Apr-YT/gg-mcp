@echo off
rem run_tests.bat - local unit tests (version family + wire protocol + ELF symbols + search + watch)
rem NOTE: ASCII-only on purpose. cmd parses .bat in the ANSI codepage (GBK here);
rem       UTF-8 Chinese comments would swallow the ^ line-continuations and break the script.
setlocal
set "ROOT=%~dp0..\.."
if not exist "%ROOT%\build\pc" mkdir "%ROOT%\build\pc"

gcc -O2 -Wall -I"%ROOT%" -o "%ROOT%\build\pc\test_version.exe" ^
  "%ROOT%\tests\unit\test_version.c" "%ROOT%\agentd\version.c"
if errorlevel 1 ( echo [FAIL] test_version build & exit /b 1 )

g++ -std=c++17 -O2 -Wall -I"%ROOT%\third_party" -I"%ROOT%\shared" ^
  -o "%ROOT%\build\pc\test_wire.exe" "%ROOT%\tests\unit\test_wire.cpp"
if errorlevel 1 ( echo [FAIL] test_wire build & exit /b 1 )

gcc -O2 -Wall -I"%ROOT%" -o "%ROOT%\build\pc\test_elfsym.exe" ^
  "%ROOT%\tests\unit\test_elfsym.c" "%ROOT%\agentd\elfsym.c"
if errorlevel 1 ( echo [FAIL] test_elfsym build & exit /b 1 )

echo === test_version ===
"%ROOT%\build\pc\test_version.exe"
if errorlevel 1 ( echo [FAIL] test_version & exit /b 1 )
echo === test_wire ===
"%ROOT%\build\pc\test_wire.exe"
if errorlevel 1 ( echo [FAIL] test_wire & exit /b 1 )
echo === test_elfsym ===
if exist "%ROOT%\build\native\libKernelGg.so" (
  "%ROOT%\build\pc\test_elfsym.exe" "%ROOT%\build\native\libKernelGg.so" "%ROOT%\build\native\lib5_stub"
) else (
  "%ROOT%\build\pc\test_elfsym.exe"
)
if errorlevel 1 ( echo [FAIL] test_elfsym & exit /b 1 )

gcc -O2 -Wall -I"%ROOT%" -I"%ROOT%\agentd" -I"%ROOT%\shared" ^
  -o "%ROOT%\build\pc\test_search.exe" "%ROOT%\tests\unit\test_search.c" "%ROOT%\agentd\search.c" -lpthread
if errorlevel 1 ( echo [FAIL] test_search build & exit /b 1 )
echo === test_search ===
"%ROOT%\build\pc\test_search.exe"
if errorlevel 1 ( echo [FAIL] test_search & exit /b 1 )

rem AoB cross-block/fault-page: search_aob carry buffer
gcc -O2 -Wall -I"%ROOT%" -I"%ROOT%\agentd" -I"%ROOT%\shared" ^
  -o "%ROOT%\build\pc\test_aob.exe" "%ROOT%\tests\unit\test_aob.c" "%ROOT%\agentd\search.c" -lpthread
if errorlevel 1 ( echo [FAIL] test_aob build & exit /b 1 )

rem multi-region parallel sharding: search_exact thread merge correctness
gcc -O2 -Wall -I"%ROOT%" -I"%ROOT%\agentd" -I"%ROOT%\shared" ^
  -o "%ROOT%\build\pc\test_parallel.exe" "%ROOT%\tests\unit\test_parallel.c" "%ROOT%\agentd\search.c" -lpthread
if errorlevel 1 ( echo [FAIL] test_parallel build & exit /b 1 )
echo === test_parallel ===
"%ROOT%\build\pc\test_parallel.exe"
if errorlevel 1 ( echo [FAIL] test_parallel & exit /b 1 )
echo === test_aob ===
"%ROOT%\build\pc\test_aob.exe"
if errorlevel 1 ( echo [FAIL] test_aob & exit /b 1 )

rem T2 maps parsing guardrail: parse_maps_line pure function (single source of truth),
rem covers file maps / anonymous (path blanked) / special segs / paths with spaces / malformed lines.
gcc -O2 -Wall -I"%ROOT%" -I"%ROOT%\agentd" ^
  -o "%ROOT%\build\pc\test_process_maps.exe" "%ROOT%\tests\unit\test_process_maps.c" "%ROOT%\agentd\process.c" -lpthread
if errorlevel 1 ( echo [FAIL] test_process_maps build & exit /b 1 )
echo === test_process_maps ===
"%ROOT%\build\pc\test_process_maps.exe"
if errorlevel 1 ( echo [FAIL] test_process_maps & exit /b 1 )

rem S1 injection guardrail: shell_split_argv / shell_argv_allowed shared with agentd/shell_util.h
gcc -O2 -Wall -I"%ROOT%" -I"%ROOT%\agentd" ^
  -o "%ROOT%\build\pc\test_shell_security.exe" "%ROOT%\tests\unit\test_shell_security.c"
if errorlevel 1 ( echo [FAIL] test_shell_security build & exit /b 1 )
echo === test_shell_security ===
"%ROOT%\build\pc\test_shell_security.exe"
if errorlevel 1 ( echo [FAIL] test_shell_security & exit /b 1 )

rem T3 freeze registry guardrail: add (new / same-addr update / oversize / full-256 reject)
rem / remove / clear / count pure list logic; protects the freeze-value feature.
gcc -O2 -Wall -I"%ROOT%" -I"%ROOT%\agentd" -I"%ROOT%\shared" ^
  -o "%ROOT%\build\pc\test_freeze.exe" "%ROOT%\tests\unit\test_freeze.c" "%ROOT%\agentd\freeze.c" -lpthread
if errorlevel 1 ( echo [FAIL] test_freeze build & exit /b 1 )
echo === test_freeze ===
"%ROOT%\build\pc\test_freeze.exe"
if errorlevel 1 ( echo [FAIL] test_freeze & exit /b 1 )

rem C5 downgrade state machine guardrail: single failure must not permanently downgrade
rem the KO driver (false-kill fix); only consecutive failures past the threshold downgrade.
gcc -O2 -Wall -I"%ROOT%\native\hook" ^
  -o "%ROOT%\build\pc\test_ko_downgrade.exe" "%ROOT%\tests\unit\test_ko_downgrade.c"
if errorlevel 1 ( echo [FAIL] test_ko_downgrade build & exit /b 1 )
echo === test_ko_downgrade ===
"%ROOT%\build\pc\test_ko_downgrade.exe"
if errorlevel 1 ( echo [FAIL] test_ko_downgrade & exit /b 1 )

rem T6 watch dedup cursor guardrail: rwproc path rwproc_hit_consumed[] reports only new hits,
rem cap-truncated remainder refilled, per-bp independent cursors; ptrace path keeps remainder (fix).
rem Needs posix_stub (MinGW lacks sys/ptrace.h sys/uio.h sys/wait.h signal.h).
gcc -O2 -Wall -I"%ROOT%\tests\unit\posix_stub" -I"%ROOT%" -I"%ROOT%\agentd" -I"%ROOT%\shared" ^
  -o "%ROOT%\build\pc\test_watch.exe" "%ROOT%\tests\unit\test_watch.c" "%ROOT%\agentd\watch.c" -lpthread
if errorlevel 1 ( echo [FAIL] test_watch build & exit /b 1 )
echo === test_watch ===
"%ROOT%\build\pc\test_watch.exe"
if errorlevel 1 ( echo [FAIL] test_watch & exit /b 1 )

rem POSIX branch syntax guardrail: local build takes _WIN32, real wire.cpp POSIX timeout branches
rem cannot compile here; use a minimal stub header to -fsyntax-only an X1 timeout logic copy.
g++ -std=c++17 -fsyntax-only -Wall -I"%ROOT%\tests\unit\posix_stub" ^
  "%ROOT%\tests\unit\test_wire_posix_syntax.cpp"
if errorlevel 1 ( echo [FAIL] test_wire_posix_syntax & exit /b 1 )

rem dispatch consistency guardrail: schema.cpp (43-tool authoritative list) cross-checked
rem against session.cpp (call_tool dispatch) to catch "new/renamed tool wired in only one place".
set "PYEXE="
where python >nul 2>nul && set "PYEXE=python"
if not defined PYEXE ( set "PYEXE=C:\Users\AprYT\.workbuddy\binaries\python\versions\3.13.12\python.exe" )
rem UTF-8 mode: python output must not die on non-GBK chars under cmd ANSI codepage
  set "PYTHONUTF8=1"
  set "PYTHONIOENCODING=utf-8"
if exist "%PYEXE%" (
  echo === dispatch consistency: schema vs session ===
  "%PYEXE%" "%ROOT%\tests\unit\check_dispatch_consistency.py" "%ROOT%"
  if errorlevel 1 ( echo [FAIL] dispatch consistency & exit /b 1 )
  echo === kernel-only guardrail: driver.c stays kernel-side ===
  "%PYEXE%" "%ROOT%\tests\unit\check_kernel_only.py" "%ROOT%"
  if errorlevel 1 ( echo [FAIL] kernel-only guardrail & exit /b 1 )
) else (
  echo [WARN] python unavailable, skipping structural guardrails
)

echo [OK] all unit tests passed
endlocal