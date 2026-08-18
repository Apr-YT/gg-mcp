# verify.ps1 — 连上后的一键端到端验证（对 PC 端 MCP 服务器发 HTTP 请求）
# 用法:
#   powershell -ExecutionPolicy Bypass -File scripts\verify.ps1 [-Url http://127.0.0.1:8788/mcp]
#   （先启动 gg-mcp-server --transport http --port 8788）
param([string]$Url = "http://127.0.0.1:8788/mcp")

$ErrorActionPreference = "Stop"
$seq = 0

function Mcp-Call {
    param([string]$Name, $Argz = @{})
    $script:seq++
    $body = @{
        jsonrpc = "2.0"
        id      = $script:seq
        method  = "tools/call"
        params  = @{ name = $Name; arguments = $Argz }
    } | ConvertTo-Json -Depth 6 -Compress
    try {
        $r = Invoke-WebRequest -Uri $Url -Method POST -Body $body -ContentType "application/json" -UseBasicParsing -TimeoutSec 120
        $j = $r.Content | ConvertFrom-Json
        $text = $j.result.content[0].text
        $parsed = $text | ConvertFrom-Json
        return $parsed
    } catch {
        Write-Host "  [HTTP错误] $($_.Exception.Message)" -ForegroundColor Red
        return $null
    }
}

function Section($t) { Write-Host "`n===== $t =====" -ForegroundColor Cyan }

Section "1) 设备信息"
$di = Mcp-Call "gg_device_info" @{}
if ($di) { $di | ConvertTo-Json -Depth 4 }

Section "2) 内核通道 (QX 驱动) — 期望 ko_loaded=true, state=kernel-driver"
$ks = Mcp-Call "gg_kernel_status" @{}
if ($ks) { $ks | ConvertTo-Json -Depth 4 }

Section "3) GG 核心目录 — 期望看到 com.tencent.gamereva 与 101.1"
$cv = Mcp-Call "gg_core_versions" @{}
if ($cv) { $cv | ConvertTo-Json -Depth 5 }

Section "4) 加载 GG 核心（版本族 101.*）"
$ld = Mcp-Call "gg_kernel_load_gg" @{ pattern = "101.*"; strict = $true }
if ($ld) { $ld | ConvertTo-Json -Depth 5 }

Section "5) 进程列表（前 10）"
$lp = Mcp-Call "gg_list_processes" @{}
if ($lp -and $lp.processes) { $lp.processes | Select-Object -First 10 | Format-Table pid, name, uid -AutoSize }

Write-Host "`n===== 验证完成 =====" -ForegroundColor Green
Write-Host "把 1~4 的输出贴给开发确认即可。第 5 步确认能列出游戏进程后，下一步是 attach + 搜索。"
