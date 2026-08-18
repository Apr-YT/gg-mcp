# ================= 部署编排（PC 侧 PowerShell，KO 驱动方案）=================
# 用法:
#   powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 [-Serial <设备>] [-SkipKo]
#   powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -GgApk "C:\Users\...\宇航员修改器.zip"
#     -GgApk: 可选，自动安装 GG 本体（支持 .apk 或含 apk 的 .zip）
param(
    [string]$Serial = "",
    [switch]$SkipKo,
    [string]$GgApk = ""
)
$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$Adb = "adb"
$DeviceArg = if ($Serial) { @("-s", $Serial) } else { @() }
$Port = 19100
$DevDir = "/data/local/tmp/ggmcp"

function Adb-Sh {
    param([string]$Cmd)
    # 远端命令后追加 RC 回显，adb shell 成功(退出0)不代表远端命令成功，
    # 必须解析远端退出码才能检测 chmod/启动失败等。注意: $Cmd 不得含单引号(会与外层冲突)。
    $remote = "sh -c '" + $Cmd + "; echo GG_ADB_RC=`$?'"
    $out = & $Adb @DeviceArg shell $remote 2>&1
    if ($LASTEXITCODE -ne 0) { throw "adb shell 失败: $Cmd" }
    $rcLine = ($out | Select-String "GG_ADB_RC=(\d+)")
    if ($rcLine) {
        $rc = [int]$rcLine.Matches[0].Groups[1].Value
        if ($rc -ne 0) { throw "远端命令失败(RC=$rc): $Cmd" }
    }
    $out | Where-Object { $_ -notmatch "GG_ADB_RC=" }
}

# 原生命令包装: push/forward/install 等失败(LASTEXITCODE!=0)直接抛错中止部署。
# 关键: PowerShell 的 $ErrorActionPreference=Stop 对原生命令(adb)退出码无效，
# 必须显式检查 LASTEXITCODE，否则 USB 掉线时部署会半截静默失败。
# 注意: 参数名不能用 $Args（自动变量, pwsh 7 下参数绑定会失效, 实参被吞）。
function Adb-Cmd {
    param([Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$AdbArgs)
    & $Adb @DeviceArg @AdbArgs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "adb 失败: $($AdbArgs -join ' ')" }
}

Write-Host "== GG-MCP 部署（KO 驱动）==" -ForegroundColor Cyan

# 0) 设备检查（多设备且无 -Serial 时拒绝，避免误操作）
$devLines = & $Adb devices | Select-String "\tdevice$"
if (-not $devLines) { throw "未检测到设备（adb devices 无 device 状态）" }
if ($devLines.Count -gt 1 -and -not $Serial) {
    throw "检测到 $($devLines.Count) 台设备，请加 -Serial <设备> 指定目标"
}

# 0.5) 可选: 安装 GG 本体（宇航员修改器 = GG 101.1, 包名 com.tencent.gamereva）
if ($GgApk) {
    Write-Host "[0.5/5] 安装 GG 本体" -ForegroundColor Yellow
    $apkPath = $GgApk
    if ($GgApk -like "*.zip") {
        $zipDir = "$Root\build\gg-install"
        if (Test-Path $zipDir) { Remove-Item $zipDir -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $zipDir | Out-Null
        Expand-Archive -Path $GgApk -DestinationPath $zipDir -Force
        $apkPath = Get-ChildItem $zipDir -Filter *.apk | Select-Object -First 1 -ExpandProperty FullName
        if (-not $apkPath) { throw "zip 里没有 apk" }
    }
    Write-Host "  adb install -r $apkPath"
    & $Adb @DeviceArg install -r $apkPath
    if ($LASTEXITCODE -ne 0) { Write-Host "  安装失败（可手动 adb install -r）" -ForegroundColor DarkYellow }
    else { Write-Host "  安装成功。首次运行 GG 后再用 gg_kernel_load_gg 对接核心" -ForegroundColor Green }
}

# 1) 推送设备端产物
Write-Host "[1/5] 推送 agentd + native 产物" -ForegroundColor Yellow
# 中文路径兼容: 控制台 UTF-8 + PS5.1 调 native(adb) 时参数按 UTF-8 传、adb 按 ANSI 解,
# 中文路径(如仓库在 "工具\")必然乱码。先把产物暂存到无中文临时目录再 push。
$Stage = Join-Path $env:TEMP "ggmcp_stage"
New-Item -ItemType Directory -Force -Path $Stage | Out-Null
Copy-Item "$Root\build\agentd\gg-agentd" $Stage -Force
Copy-Item "$Root\build\native\libKernelGg.so" $Stage -Force
Copy-Item "$Root\build\native\lib5_stub" $Stage -Force
Copy-Item "$Root\scripts\start_agentd.sh" $Stage -Force
Copy-Item "$Root\scripts\install_ko.sh" $Stage -Force
Adb-Sh "mkdir -p $DevDir/ko" | Out-Null
Adb-Cmd "push" "$Stage\gg-agentd" "$DevDir/"
Adb-Cmd "push" "$Stage\libKernelGg.so" "$DevDir/"
Adb-Cmd "push" "$Stage\lib5_stub" "$DevDir/"
Adb-Cmd "push" "$Stage\start_agentd.sh" "$DevDir/"
Adb-Cmd "push" "$Stage\install_ko.sh" "$DevDir/"
Adb-Sh "chmod 755 $DevDir/gg-agentd $DevDir/lib5_stub $DevDir/start_agentd.sh $DevDir/install_ko.sh"
Adb-Sh "chmod 777 $DevDir/libKernelGg.so"

# 2) KO 内核驱动（可跳过）
if (-not $SkipKo) {
    Write-Host "[2/5] 加载 KO 内核驱动" -ForegroundColor Yellow
    $koFiles = Get-ChildItem "$Root\build\ko" -Filter *.ko -ErrorAction SilentlyContinue
    if ($koFiles) {
        # ko/ 可能被历史部署(root)占用, adb shell 用户无写权限 → 先提权放开
        Adb-Sh "su -c ""chmod -R 777 $DevDir"""
        New-Item -ItemType Directory -Force -Path "$Stage\ko" | Out-Null
        foreach ($f in $koFiles) { Copy-Item $f.FullName "$Stage\ko\" -Force }
        foreach ($f in $koFiles) { Adb-Cmd "push" "$Stage\ko\$($f.Name)" "$DevDir/ko/" }
        Adb-Sh "su -c ""sh $DevDir/install_ko.sh""" | ForEach-Object { Write-Host "  $_" }
    } else {
        Write-Host "  build\ko\ 下没有 .ko 文件，跳过加载。"
        Write-Host "  把你的 KO 驱动（如 driverQX114_6.12.9.ko）放入 build\ko\ 后重跑本脚本" -ForegroundColor DarkYellow
    }
}

# 3) 启动 agentd
Write-Host "[3/5] 启动 gg-agentd" -ForegroundColor Yellow
Adb-Sh "su -c ""sh $DevDir/start_agentd.sh stop""" | Out-Null
Adb-Sh "su -c ""sh $DevDir/start_agentd.sh start""" | ForEach-Object { Write-Host "  $_" }

# 4) adb forward
Write-Host "[4/5] adb forward tcp:$Port" -ForegroundColor Yellow
Adb-Cmd "forward" "tcp:$Port" "tcp:$Port"

# 5) 自检日志（轮询就绪标记，替代固定短 sleep；KO 加载慢时不漏判）
Write-Host "[5/5] agentd 自检日志" -ForegroundColor Yellow
$ready = $false
for ($i = 0; $i -lt 10; $i++) {
    $log = & $Adb @DeviceArg shell "tail -n 20 $DevDir/gg-agentd.log 2>/dev/null"
    if ($log -match "listening|started|ready|running|port") { $ready = $true; break }
    Start-Sleep -Milliseconds 500
}
& $Adb @DeviceArg shell "tail -n 20 $DevDir/gg-agentd.log 2>/dev/null" | ForEach-Object { Write-Host "  $_" }
if (-not $ready) { Write-Host "  警告: 未检测到就绪标记，仍请检查日志确认 agentd 已起" -ForegroundColor DarkYellow }

Write-Host ""
Write-Host "== 部署完成 ==" -ForegroundColor Green
Write-Host "启动 PC 端 MCP 服务器:"
Write-Host "  build\pc\gg-mcp-server.exe --config configs\ggmcp.json --transport http --port 8788"
Write-Host "然后调 gg_kernel_status 验证: hook 型期望 state=ko-hook / dev 型期望 state=kernel-driver"
