@echo off
rem run_tests.bat - local unit tests (version family + wire protocol + ELF symbols)
setlocal
set ROOT=%~dp0..\..
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
  -o "%ROOT%\build\pc\test_search.exe" "%ROOT%\tests\unit\test_search.c" "%ROOT%\agentd\search.c"
if errorlevel 1 ( echo [FAIL] test_search build & exit /b 1 )
echo === test_search ===
"%ROOT%\build\pc\test_search.exe"
if errorlevel 1 ( echo [FAIL] test_search & exit /b 1 )

rem AoB cross-block/fault-page: search_aob carry buffer
gcc -O2 -Wall -I"%ROOT%" -I"%ROOT%\agentd" -I"%ROOT%\shared" ^
  -o "%ROOT%\build\pc\test_aob.exe" "%ROOT%\tests\unit\test_aob.c" "%ROOT%\agentd\search.c"
if errorlevel 1 ( echo [FAIL] test_aob build & exit /b 1 )
echo === test_aob ===
"%ROOT%\build\pc\test_aob.exe"
if errorlevel 1 ( echo [FAIL] test_aob & exit /b 1 )

rem T2 maps 解析护栏：process_maps 抽取的 parse_maps_line 纯函数（单一事实源），
rem 覆盖文件映射/匿名(path 置空)/特殊段/带空格路径/畸形行；防搜索 region 过滤回归。
gcc -O2 -Wall -I"%ROOT%" -I"%ROOT%\agentd" ^
  -o "%ROOT%\build\pc\test_process_maps.exe" "%ROOT%\tests\unit\test_process_maps.c" "%ROOT%\agentd\process.c"
if errorlevel 1 ( echo [FAIL] test_process_maps build & exit /b 1 )
echo === test_process_maps ===
"%ROOT%\build\pc\test_process_maps.exe"
if errorlevel 1 ( echo [FAIL] test_process_maps & exit /b 1 )

rem S1 注入防护护栏：共享 agentd/shell_util.h 的 shell_split_argv/shell_argv_allowed
gcc -O2 -Wall -I"%ROOT%" -I"%ROOT%\agentd" ^
  -o "%ROOT%\build\pc\test_shell_security.exe" "%ROOT%\tests\unit\test_shell_security.c"
if errorlevel 1 ( echo [FAIL] test_shell_security build & exit /b 1 )
echo === test_shell_security ===
"%ROOT%\build\pc\test_shell_security.exe"
if errorlevel 1 ( echo [FAIL] test_shell_security & exit /b 1 )

rem T3 freeze 注册表护栏：freeze_add（新增/同地址更新/超长/满额 256 拒绝）
rem /remove/clear/count 纯列表逻辑；防冻结值功能回归。
gcc -O2 -Wall -I"%ROOT%" -I"%ROOT%\agentd" -I"%ROOT%\shared" ^
  -o "%ROOT%\build\pc\test_freeze.exe" "%ROOT%\tests\unit\test_freeze.c" "%ROOT%\agentd\freeze.c" -lpthread
if errorlevel 1 ( echo [FAIL] test_freeze build & exit /b 1 )
echo === test_freeze ===
"%ROOT%\build\pc\test_freeze.exe"
if errorlevel 1 ( echo [FAIL] test_freeze & exit /b 1 )

rem C5 降级状态机护栏：单次请求失败不永久降级 KO 驱动（误杀修复），
rem 仅连续失败达阈值才降级；守护 native/hook/ko_downgrade.h 逻辑。
gcc -O2 -Wall -I"%ROOT%\native\hook" ^
  -o "%ROOT%\build\pc\test_ko_downgrade.exe" "%ROOT%\tests\unit\test_ko_downgrade.c"
if errorlevel 1 ( echo [FAIL] test_ko_downgrade build & exit /b 1 )
echo === test_ko_downgrade ===
"%ROOT%\build\pc\test_ko_downgrade.exe"
if errorlevel 1 ( echo [FAIL] test_ko_downgrade & exit /b 1 )

rem T6 watch 去重游标护栏：rwproc 路径 rwproc_hit_consumed[] 只上报新增命中、
rem cap 截断剩余补齐、多 bp 独立游标；ptrace 路径 cap 截断剩余保留（修复）。
rem 依赖 posix_stub（MinGW 缺 sys/ptrace.h/sys/uio.h/sys/wait.h/signal.h）。
gcc -O2 -Wall -I"%ROOT%\tests\unit\posix_stub" -I"%ROOT%" -I"%ROOT%\agentd" -I"%ROOT%\shared" ^
  -o "%ROOT%\build\pc\test_watch.exe" "%ROOT%\tests\unit\test_watch.c" "%ROOT%\agentd\watch.c" -lpthread
if errorlevel 1 ( echo [FAIL] test_watch build & exit /b 1 )
echo === test_watch ===
"%ROOT%\build\pc\test_watch.exe"
if errorlevel 1 ( echo [FAIL] test_watch & exit /b 1 )

rem POSIX 分支语法护栏：本机走 _WIN32，无法编译真实 wire.cpp 的 POSIX 超时分支；
rem 用极简 stub 头对 X1 超时逻辑副本做 -fsyntax-only，防止 POSIX 代码语法回归。
g++ -std=c++17 -fsyntax-only -Wall -I"%ROOT%\tests\unit\posix_stub" ^
  "%ROOT%\tests\unit\test_wire_posix_syntax.cpp"
if errorlevel 1 ( echo [FAIL] test_wire_posix_syntax & exit /b 1 )

rem 分发一致性护栏：schema.cpp(43工具权威清单) ↔ session.cpp(call_tool 分发) 自动交叉校验
rem 防「新增/重命名工具漏改一处导致调用即 unknown」或「孤儿分支/拼写漂移」。零编译零风险。
set "PYEXE="
where python >nul 2>nul && set "PYEXE=python"
if not defined PYEXE ( set "PYEXE=C:\Users\AprYT\.workbuddy\binaries\python\versions\3.13.12\python.exe" )
if exist "%PYEXE%" (
  echo === dispatch consistency (schema vs session) ===
  "%PYEXE%" "%ROOT%\tests\unit\check_dispatch_consistency.py" "%ROOT%"
  if errorlevel 1 ( echo [FAIL] dispatch consistency & exit /b 1 )
  echo === kernel-only guardrail (driver.c 不落用户态) ===
  "%PYEXE%" "%ROOT%\tests\unit\check_kernel_only.py" "%ROOT%"
  if errorlevel 1 ( echo [FAIL] kernel-only guardrail & exit /b 1 )
) else (
  echo [WARN] python 不可用，跳过检查类护栏
)

echo [OK] all unit tests passed
endlocal
