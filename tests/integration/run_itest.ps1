# run_itest.ps1 — 设备端集成测试（需已 root 设备 + adb）
# 编译 itest → push → 以 root 运行 → 收集结果
# adb 会把推送进度、守护进程提示写到 stderr。PS 5.1 在 $ErrorActionPreference='Stop'
# 下会把 native 命令的 stderr 直接包装成终止性错误 —— 表现为「推送明明成功却报错退出」。
# 因此这里统一用 Continue，真正的失败一律由 $LASTEXITCODE / itest 退出码判定。
$ErrorActionPreference = "Continue"
# 2026-10-15 fix: $PSScriptRoot = <repo>\tests\integration, so a single -Parent
# yields <repo>\tests and every "$Root\agentd\..." path missed by one level
# (it looked for <repo>\tests\tests\integration\itest.c). Go up two levels.
$Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Adb = "adb"
if (-not (Get-Command $Adb -ErrorAction SilentlyContinue)) {
    # adb is on PATH on the dev box, but fall back to the known install dir
    $AdbFallback = "D:\Kiln\platform-tools\adb.exe"
    if (Test-Path $AdbFallback) { $Adb = $AdbFallback }
    else { throw "adb 未找到（PATH 与 $AdbFallback 均不可用）" }
}
$DevDir = "/data/local/tmp/ggmcp"
$BuildNative = Join-Path $Root "build\native"
if (-not (Test-Path $BuildNative)) { New-Item -ItemType Directory -Force -Path $BuildNative | Out-Null }

# 1) NDK 编译
$cc = "D:\Kiln\android-ndk-r30\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android28-clang.cmd"
if (-not (Test-Path $cc)) { throw "NDK clang 未找到: $cc" }
& $cc -O2 -Wall -I"$Root" -o "$Root\build\native\itest" `
    "$Root\tests\integration\itest.c" `
    "$Root\agentd\driver.c" "$Root\agentd\process.c" "$Root\agentd\search.c" `
    "$Root\agentd\freeze.c" "$Root\agentd\version.c" `
    "$Root\third_party\cJSON.c" -pie
if ($LASTEXITCODE -ne 0) { throw "itest 编译失败" }

# 2) push + 运行
& $Adb shell "mkdir -p $DevDir" 2>$null | Out-Null
& $Adb push "$Root\build\native\itest" "$DevDir/itest" 2>$null | Out-Null
& $Adb shell "chmod 755 $DevDir/itest" 2>$null | Out-Null
Write-Host "== 设备端集成测试 ==" -ForegroundColor Cyan
# itest 需要 rwproc 的两个 proc 节点 key：模块默认 hide_by_default，
# 节点在 /proc 里能扫到但目录名不匹配时驱动找不到，必须显式给。
# su -c 不继承调用方环境变量，所以直接把 key 拼进远端命令行。
$memKey = ""; $bpKey = ""
$sas = Join-Path $Root "scripts\start_agentd.sh"
if (Test-Path $sas) {
    # 必须显式 -Encoding UTF8：PS 5.1 默认按 ANSI(GBK) 读，含中文注释的 UTF-8
    # 文件会被解析成乱码并把多行粘成一行，正则于是永远匹配不到。
    foreach ($line in (Get-Content $sas -Encoding UTF8)) {
        if ($line -match '^\s*RWPROC_MEM_KEY=(\S*)') { $memKey = $Matches[1] }
        if ($line -match '^\s*RWPROC_BP_KEY=(\S*)')  { $bpKey = $Matches[1] }
    }
}
if (-not $memKey) { Write-Host "  警告: 未从 start_agentd.sh 读到 RWPROC_MEM_KEY，itest 可能找不到节点" -ForegroundColor DarkYellow }
$remote = if ($memKey) { "GG_MEM_KEY=$memKey GG_BP_KEY=$bpKey $DevDir/itest" } else { "$DevDir/itest" }
# adb 会把进度/提示写到 stderr，Stop 模式下会被当致命错误；真正失败由 itest 退出码体现
$ErrorActionPreference = 'Continue'
& $Adb shell "su -c '$remote'"
Write-Host "== 完成 ==" -ForegroundColor Green
