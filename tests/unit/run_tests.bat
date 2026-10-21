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

rem comm-layer failure semantics: real sockets, fake peer on 127.0.0.1 drives
rem AgentClient::call() -> no response / half frame / bad magic / bad version.
g++ -std=c++17 -O2 -Wall -I"%ROOT%" -I"%ROOT%\third_party" -I"%ROOT%\shared" ^
  -o "%ROOT%\build\pc\test_wire_io.exe" "%ROOT%\tests\unit\test_wire_io.cpp" ^
  "%ROOT%\server\device\wire.cpp" -lws2_32
if errorlevel 1 ( echo [FAIL] test_wire_io build & exit /b 1 )

rem session enum guardrail: put_type / put_region / put_op reject explicit bad
rem values (false, no default written) while missing fields still default.
g++ -std=c++17 -O2 -Wall -I"%ROOT%" -I"%ROOT%\third_party" -I"%ROOT%\shared" ^
  -o "%ROOT%\build\pc\test_session.exe" "%ROOT%\tests\unit\test_session.cpp" ^
  "%ROOT%\server\session.cpp" "%ROOT%\server\annotations.cpp" ^
  "%ROOT%\server\device\wire.cpp" -lws2_32
if errorlevel 1 ( echo [FAIL] test_session build & exit /b 1 )

rem results_get_page paging guard: the bulk segment reader must respect the
rem caller's limit. Search hits are mostly contiguous addresses, so without
rem that bound a single small-page read overruns out_addrs/out_values.
gcc -O2 -Wall -I"%ROOT%" -I"%ROOT%\agentd" -I"%ROOT%\shared" ^
  -o "%ROOT%\build\pc\test_results_page.exe" "%ROOT%\tests\unit\test_results_page.c" ^
  "%ROOT%\agentd\search.c"
if errorlevel 1 ( echo [FAIL] test_results_page build & exit /b 1 )

gcc -O2 -Wall -I"%ROOT%" -o "%ROOT%\build\pc\test_elfsym.exe" ^
  "%ROOT%\tests\unit\test_elfsym.c" "%ROOT%\agentd\elfsym.c"
if errorlevel 1 ( echo [FAIL] test_elfsym build & exit /b 1 )

echo === test_version ===
"%ROOT%\build\pc\test_version.exe"
if errorlevel 1 ( echo [FAIL] test_version & exit /b 1 )
echo === test_wire ===
"%ROOT%\build\pc\test_wire.exe"
if errorlevel 1 ( echo [FAIL] test_wire & exit /b 1 )
echo === test_wire_io ===
"%ROOT%\build\pc\test_wire_io.exe"
if errorlevel 1 ( echo [FAIL] test_wire_io & exit /b 1 )
echo === test_session ===
"%ROOT%\build\pc\test_session.exe"
if errorlevel 1 ( echo [FAIL] test_session & exit /b 1 )
echo === test_results_page ===
"%ROOT%\build\pc\test_results_page.exe"
if errorlevel 1 ( echo [FAIL] test_results_page & exit /b 1 )
echo === test_elfsym ===
rem The GG core injection chain (libKernelGg.so / lib5_stub) was removed on
rem 2026-10-21; test_elfsym therefore always runs against its own fixture.
"%ROOT%\build\pc\test_elfsym.exe"
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

rem dispatch consistency guardrail: schema.cpp (45-tool authoritative list) cross-checked
rem against session.cpp (call_tool dispatch) to catch "new/renamed tool wired in only one place".
rem
rem python probe (2026-10-15 fix -- both guardrails below used to be DEAD CODE):
rem   old code:  where python >nul 2>nul && set "PYEXE=python"
rem              if exist "%PYEXE%" ( ...run... ) else ( echo [WARN] skipping )
rem   `if exist` resolves a FILE relative to the cwd -- it does NOT do a PATH lookup.
rem   So with PYEXE=python the [WARN] branch always won and BOTH structural guardrails
rem   were silently skipped on every machine (local and CI), reporting a false green.
rem   Now: probe to a FULL PATH, require a real python 3, and FAIL loudly if none is
rem   found instead of passing silently. Override with GG_SKIP_GUARDRAILS=1 if needed.
set "PYEXE="
call :probe_py python
if not defined PYEXE call :probe_py python3
if not defined PYEXE call :probe_py py
if not defined PYEXE if defined GG_PYTHON call :probe_py "%GG_PYTHON%"
if not defined PYEXE (
  for /d %%d in ("%LOCALAPPDATA%\Programs\Python\Python3*") do if not defined PYEXE call :probe_py "%%d\python.exe"
)
if not defined PYEXE (
  for /d %%d in ("%USERPROFILE%\.workbuddy\binaries\python\versions\*") do if not defined PYEXE call :probe_py "%%d\python.exe"
)
if not defined PYEXE (
  for /d %%d in ("%ProgramFiles%\Python3*") do if not defined PYEXE call :probe_py "%%d\python.exe"
)

rem UTF-8 mode: python output must not die on non-GBK chars under cmd ANSI codepage
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"

rem NOTE: branch with `goto`, not an if/else-if/else chain -- an unescaped ')'
rem       inside a parenthesised block silently truncates the block in cmd.
if defined PYEXE goto :gg_run_guardrails
if defined GG_SKIP_GUARDRAILS goto :gg_skip_guardrails
goto :gg_no_guardrails

:gg_run_guardrails
echo === dispatch consistency: schema vs session ===
"%PYEXE%" "%ROOT%\tests\unit\check_dispatch_consistency.py" "%ROOT%"
if errorlevel 1 ( echo [FAIL] dispatch consistency & exit /b 1 )
echo === kernel-only guardrail: driver.c stays kernel-side ===
"%PYEXE%" "%ROOT%\tests\unit\check_kernel_only.py" "%ROOT%"
if errorlevel 1 ( echo [FAIL] kernel-only guardrail & exit /b 1 )
rem F-01 guard: rwproc_buf is a process-wide static buffer shared by the search
rem worker pool and the freeze thread. Its request/result copies must stay inside
rem the mutex, otherwise threads overwrite each other's data (silent corruption).
echo === rwproc locking guardrail: shared buffer inside the critical section ===
"%PYEXE%" "%ROOT%\tests\unit\check_rwproc_locking.py" "%ROOT%"
if errorlevel 1 ( echo [FAIL] rwproc locking guardrail & exit /b 1 )
goto :gg_guardrails_done

:gg_skip_guardrails
echo [WARN] python not found - structural guardrails SKIPPED because GG_SKIP_GUARDRAILS is set
goto :gg_guardrails_done

:gg_no_guardrails
echo [FAIL] python 3 not found: structural guardrails cannot run.
echo        They catch "tool declared but not dispatched" and
echo        "user-level fallback crept back into driver.c" - do not merge blind.
echo        Fix: install Python 3, or set GG_PYTHON to the full path of python.exe,
echo        or set GG_SKIP_GUARDRAILS=1 to accept the skip explicitly.
exit /b 1

:gg_guardrails_done

echo [OK] all unit tests passed
endlocal
exit /b 0

:probe_py
rem %~1 = command name (PATH lookup) or full path. Accept only python 3; sets PYEXE.
set "CAND="
if "%~1"=="" exit /b 0
if exist "%~1" (
  set "CAND=%~1"
) else (
  for /f "delims=" %%p in ('where "%~1" 2^>nul') do if not defined CAND set "CAND=%%p"
)
if not defined CAND exit /b 0
"%CAND%" -c "import sys;sys.exit(0 if sys.version_info[0]==3 else 1)" >nul 2>nul
if not errorlevel 1 set "PYEXE=%CAND%"
exit /b 0