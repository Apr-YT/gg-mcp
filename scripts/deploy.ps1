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
$Port = 34127
$DevDir = "/data/adb/GG-Ghost"

function Adb-Sh {
    param([string]$Cmd)
    # adb 会把进度/提示写到 stderr。在 $ErrorActionPreference='Stop' 下，
    # PowerShell 会把 native 命令的 stderr 当成 ErrorRecord 直接终止脚本（假失败）。
    # 这里在函数作用域内降级为 Continue，真正的失败仍由下面的 $LASTEXITCODE 判定。
    $ErrorActionPreference = 'Continue'
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
    # 同 Adb-Sh：adb 的 stderr（推送进度等）不能被当成致命错误。
    $ErrorActionPreference = 'Continue'
    & $Adb @DeviceArg @AdbArgs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "adb 失败: $($AdbArgs -join ' ')" }
}

Write-Host "== GG-MCP 部署（KO 驱动）==" -ForegroundColor Cyan

# 0) 设备检查（多设备且无 -Serial 时拒绝，避免误操作）
# PS 5.1 会把 native 命令的 stderr（adb daemon 启动提示等）包装成终止性错误，
# 且 2>$null 也拦不住 —— 暂时降级为 Continue，这段结束后立刻恢复 Stop。
$ErrorActionPreference = 'Continue'
$devLines = & $Adb devices 2>&1 | Select-String "\tdevice$"
$ErrorActionPreference = 'Stop'
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
Write-Host "[1/6] 推送 agentd + native 产物" -ForegroundColor Yellow
# 先停掉正在运行的 agentd：运行中的二进制无法被覆盖（Text file busy）。
# 首次部署时脚本还不存在，失败属正常，直接吞掉。
try { Adb-Sh "su -c ""sh $DevDir/start_agentd.sh stop""" | Out-Null } catch { }
# 中文路径兼容: 控制台 UTF-8 + PS5.1 调 native(adb) 时参数按 UTF-8 传、adb 按 ANSI 解,
# 中文路径(如仓库在 "工具\")必然乱码。先把产物暂存到无中文临时目录再 push。
$Stage = Join-Path $env:TEMP "ggmcp_stage"
New-Item -ItemType Directory -Force -Path $Stage | Out-Null
Copy-Item "$Root\build\agentd\gg-agentd" "$Stage\gdm" -Force
Copy-Item "$Root\build\native\libKernelGg.so" $Stage -Force
Copy-Item "$Root\build\native\lib5_stub" $Stage -Force
Copy-Item "$Root\scripts\start_agentd.sh" $Stage -Force
Copy-Item "$Root\scripts\install_ko.sh" $Stage -Force
Adb-Sh "su -c ""mkdir -p $DevDir/ko""" | Out-Null
# /data/adb 是 root 0700，adb shell 跑在 shell uid(2000)：既不能 mkdir 也不能 push。
# 必须先推到 shell 可写的 /data/local/tmp，再用 su 复制过去并设权限。
$TmpDir = "/data/local/tmp/ggmcp_stage"
Adb-Sh "mkdir -p $TmpDir" | Out-Null
Adb-Cmd "push" "$Stage\gdm" "$TmpDir/"
Adb-Cmd "push" "$Stage\libKernelGg.so" "$TmpDir/"
Adb-Cmd "push" "$Stage\lib5_stub" "$TmpDir/"
Adb-Cmd "push" "$Stage\start_agentd.sh" "$TmpDir/"
Adb-Cmd "push" "$Stage\install_ko.sh" "$TmpDir/"
# 逐条 su 执行，不要拼成 && 长串：PowerShell + adb 的多层引号穿透会把 && 吃掉，
# 表现为整条 su -c 返回 1 而单条命令其实都正常。
Adb-Sh "su -c ""cp $TmpDir/gdm $DevDir/gdm""" | Out-Null
Adb-Sh "su -c ""cp $TmpDir/libKernelGg.so $DevDir/libKernelGg.so""" | Out-Null
Adb-Sh "su -c ""cp $TmpDir/lib5_stub $DevDir/lib5_stub""" | Out-Null
Adb-Sh "su -c ""cp $TmpDir/start_agentd.sh $DevDir/start_agentd.sh""" | Out-Null
Adb-Sh "su -c ""cp $TmpDir/install_ko.sh $DevDir/install_ko.sh""" | Out-Null
Adb-Sh "su -c ""chmod 755 $DevDir/gdm""" | Out-Null
Adb-Sh "su -c ""chmod 755 $DevDir/lib5_stub""" | Out-Null
Adb-Sh "su -c ""chmod 755 $DevDir/start_agentd.sh""" | Out-Null
Adb-Sh "su -c ""chmod 755 $DevDir/install_ko.sh""" | Out-Null
Adb-Sh "su -c ""chmod 777 $DevDir/libKernelGg.so""" | Out-Null

# 2) KO 内核驱动（可跳过）
if (-not $SkipKo) {
    Write-Host "[2/6] 加载 KO 内核驱动" -ForegroundColor Yellow
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
Write-Host "[3/6] 启动 agentd" -ForegroundColor Yellow
Adb-Sh "su -c ""sh $DevDir/start_agentd.sh stop""" | Out-Null
Adb-Sh "su -c ""sh $DevDir/start_agentd.sh start""" | ForEach-Object { Write-Host "  $_" }

# 4) adb forward
Write-Host "[4/6] adb forward tcp:$Port" -ForegroundColor Yellow
Adb-Cmd "forward" "tcp:$Port" "tcp:$Port"

# 5) 同步 wire 握手密钥到本地配置
#    agentd 首次启动会在 $DevDir/wire.key 生成 32 字节随机密钥（0600，仅 root 可读）。
#    PC 端必须配同一值，否则握手被拒（GGERR_PERM）。这里自动拉取并写入 configs，
#    避免手工同步出错导致的「连不上但看不出原因」。
Write-Host "[5/6] 同步 wire 握手密钥" -ForegroundColor Yellow
$keyOut = & $Adb @DeviceArg shell "su -c ""cat $DevDir/wire.key 2>/dev/null""" 2>$null
$wireKey = ($keyOut | Where-Object { $_ -match '^[0-9a-fA-F]+$' } | Select-Object -First 1)
if ($wireKey) {
    $wireKey = $wireKey.Trim()
    $cfgPath = Join-Path $Root "configs\gg-ghost.json"
    if (Test-Path $cfgPath) {
        $cfgObj = Get-Content $cfgPath -Raw | ConvertFrom-Json
        $cfgObj | Add-Member -NotePropertyName wire_key -NotePropertyValue $wireKey -Force
        # 必须写 UTF-8 无 BOM：PS5.1 的 -Encoding UTF8 会插 BOM，
        # 而 nlohmann::json 与多数 JSON 工具不跳过 BOM，会让服务端配置解析失败。
        $jsonText = $cfgObj | ConvertTo-Json -Depth 10
        [System.IO.File]::WriteAllText($cfgPath, $jsonText, (New-Object System.Text.UTF8Encoding($false)))
        Write-Host "  已写入 configs\gg-ghost.json (wire_key 长度 $($wireKey.Length))"
    } else {
        Write-Host "  警告: $cfgPath 不存在，请手动把该值写入配置 wire_key: $wireKey" -ForegroundColor DarkYellow
    }
} else {
    Write-Host "  警告: 读取 $DevDir/wire.key 失败 — agentd 可能以【无鉴权】模式运行，本机任意进程可连端口" -ForegroundColor DarkYellow
}

# 6) 自检日志（轮询就绪标记，替代固定短 sleep；KO 加载慢时不漏判）
Write-Host "[6/6] agentd 自检日志" -ForegroundColor Yellow
$ready = $false
for ($i = 0; $i -lt 10; $i++) {
    $log = & $Adb @DeviceArg shell "tail -n 20 $DevDir/agentd.log 2>/dev/null"
    if ($log -match "listening|started|ready|running|port") { $ready = $true; break }
    Start-Sleep -Milliseconds 500
}
& $Adb @DeviceArg shell "tail -n 20 $DevDir/agentd.log 2>/dev/null" | ForEach-Object { Write-Host "  $_" }
if (-not $ready) { Write-Host "  警告: 未检测到就绪标记，仍请检查日志确认 agentd 已起" -ForegroundColor DarkYellow }

Write-Host ""
Write-Host "== 部署完成 ==" -ForegroundColor Green
Write-Host "启动 PC 端 MCP 服务器:"
Write-Host "  build\pc\gg-ghost-mcp.exe --config configs\gg-ghost.json --transport http --port 8788"
Write-Host "然后调 gg_kernel_status 验证: hook 型期望 state=ko-hook / dev 型期望 state=kernel-driver"
