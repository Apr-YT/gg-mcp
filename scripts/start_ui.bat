@echo off
rem start_ui.bat - launch the PC-side gg-ui from the repo root.
rem
rem Why this wrapper exists: gg-ui resolves its config as the RELATIVE path
rem "configs\gg-ghost.json", so it must be started with the repo root as CWD.
rem Launching the exe by double-click (or from explorer) inherits a different
rem CWD and the UI comes up without a wire key -> "key mismatch" on connect.
rem
rem usage: double-click this file, or from a shell:
rem   scripts\start_ui.bat
rem   scripts\start_ui.bat --pid 32369
rem   scripts\start_ui.bat --search --value 1000 --type dword
cd /d "%~dp0.."
echo [start_ui] cwd = %CD%
start "GG-UI" "build\pc\gg-ui.exe" %*
