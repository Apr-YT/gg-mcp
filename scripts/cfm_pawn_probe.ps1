# cfm_pawn_probe.ps1 — CFM 人物基址只读探测（经 GG-MCP KO 内核通道）
# 用法: pwsh -File scripts\cfm_pawn_probe.ps1 [-McpUrl http://127.0.0.1:8788/mcp] [-TargetPid 6990]
# 约束: 全程只读（gg_read_bytes），绝不写内存；需游戏处于【对局/训练营】中才能枚举到人物
# 偏移表来源: D:\project\trae\learn\cf\offsets.md (2026-08-15, Unity5.6.4p4 IL2CPP)

param(
  [string]$McpUrl = "http://127.0.0.1:8788/mcp",
  [int]$TargetPid = 0
)

$ErrorActionPreference = "Stop"
$lib = 0
$baseAddr = 0x7a41a00000   # 占位，运行时用 gg_module_base 刷新

function Invoke-Mcp($id, $name, $argsObj) {
  $body = @{ jsonrpc = "2.0"; id = $id; method = "tools/call"; params = @{ name = $name; arguments = $argsObj } } | ConvertTo-Json -Depth 6 -Compress
  $r = Invoke-RestMethod -Uri $McpUrl -Method Post -Body $body -ContentType "application/json" -TimeoutSec 30
  if ($r.result.isError) { throw ("MCP error: " + $r.result.content[0].text) }
  return $r.result.content[0].text
}

function ReadHex($addr, $len) {
  $h = "{0:X}" -f $addr
  $t = Invoke-Mcp 90 "gg_read_bytes" @{ addr = ("0x" + $h); len = $len }
  $j = $t | ConvertFrom-Json
  return $j.hex
}

function ReadU64($addr) {
  $hx = ReadHex $addr 8
  $b = [byte[]]::new(8); for ($i = 0; $i -lt 8; $i++) { $b[$i] = [Convert]::ToByte($hx.Substring($i * 2, 2), 16) }
  return [BitConverter]::ToUInt64($b, 0)
}

function ReadU32($addr) {
  $hx = ReadHex $addr 4
  $b = [byte[]]::new(4); for ($i = 0; $i -lt 4; $i++) { $b[$i] = [Convert]::ToByte($hx.Substring($i * 2, 2), 16) }
  return [BitConverter]::ToUInt32($b, 0)
}

function ReadF32($addr) {
  $hx = ReadHex $addr 4
  $b = [byte[]]::new(4); for ($i = 0; $i -lt 4; $i++) { $b[$i] = [Convert]::ToByte($hx.Substring($i * 2, 2), 16) }
  return [BitConverter]::ToSingle($b, 0)
}

function ReadWStr16($strObj, $maxChars) {
  # IL2CPP String: +0x10 = length(int32), +0x14 = UTF-16 字符数据
  $len = ReadU32 ($strObj + 0x10)
  if ($len -le 0 -or $len -gt $maxChars) { return "" }
  $hx = ReadHex ($strObj + 0x14) ($len * 2)
  $sb = [System.Text.StringBuilder]::new()
  for ($i = 0; $i -lt $len; $i++) {
    $b0 = [Convert]::ToByte($hx.Substring($i * 4, 2), 16)
    $b1 = [Convert]::ToByte($hx.Substring($i * 4 + 2, 2), 16)
    [void]$sb.Append([char](($b1 -shl 8) -bor $b0))
  }
  return $sb.ToString()
}

function IsUserPtr($v) { return ($v -ge 0x7000000000 -and $v -lt 0x8000000000) }

# ---- 1. 附加 + 取 libil2cpp 基址 ----
Write-Host "== 1. attach & module base =="
if ($TargetPid -eq 0) {
  $procText = Invoke-Mcp 91 "gg_list_processes" @{ filter = "tmgp.cf" }
  $procs = ($procText | ConvertFrom-Json)
  $target = $procs.processes | Where-Object { $_.name -match "tmgp.cf" } | Select-Object -First 1
  if (-not $target) { throw "CFM process not found: $procText" }
  $TargetPid = $target.pid
  Write-Host ("CFM pid = {0}" -f $TargetPid)
} else {
  $null = Invoke-Mcp 92 "gg_attach" @{ pid = $TargetPid }
  Write-Host ("attached pid = {0}" -f $TargetPid)
}
$modText = Invoke-Mcp 93 "gg_module_base" @{ name = "libil2cpp.so" }
$mod = $modText | ConvertFrom-Json
$lib = [Convert]::ToUInt64($mod.base.Substring(2), 16)
Write-Host ("libil2cpp.so base = 0x{0:X}" -f $lib)

# ---- 2. WorldManager 静态锚点链 -> BaseGame -> ListWN<Pawn> ----
Write-Host "== 2. static anchor chain =="
$anch = $lib + 0xC5EDA08
$a1 = ReadU64 $anch;          Write-Host ("anchor 0xC5EDA08 -> a1      = 0x{0:X}" -f $a1)
$a2 = ReadU64 $a1;            Write-Host ("a1 -> klass                 = 0x{0:X}" -f $a2)
$sf = ReadU64 ($a2 + 0xA0);   Write-Host ("klass+0xA0 -> static_fields = 0x{0:X}" -f $sf)
$wm = ReadU64 $sf;            Write-Host ("sf -> WorldManager          = 0x{0:X}" -f $wm)
$bw = ReadU64 ($wm + 0x20);   Write-Host ("wm+0x20 -> BaseWorld        = 0x{0:X}" -f $bw)
$bg = ReadU64 ($bw + 0x28);   Write-Host ("bw+0x28 -> BaseGame         = 0x{0:X}" -f $bg)

if ($bg -eq 0) {
  Write-Host "!! BaseGame = NULL：游戏不在对局中（大厅无玩家列表）。请进入对局/训练营后重跑。"
  exit 1
}

$list = ReadU64 ($bg + 0x70); Write-Host ("BaseGame+0x70 -> ListWN    = 0x{0:X}" -f $list)
if ($list -eq 0) { Write-Host "!! ListWN = NULL：玩家列表未初始化（可能刚开局/已结束）。"; exit 1 }
$items = ReadU64 ($list + 0x10); Write-Host ("items(+0x10) = 0x{0:X}  size(+0x18) = {1}" -f $items, (ReadU32 ($list + 0x18)))
$size = ReadU32 ($list + 0x18)
if ($size -gt 128) { $size = 128 }

# ---- 3. 枚举 Pawn 并验证关键偏移 ----
Write-Host "== 3. Pawn enumeration (read-only) =="
Write-Host ("total pawns = {0}" -f $size)
for ($k = 0; $k -lt $size; $k++) {
  $pawn = ReadU64 ($items + 0x20 + $k * 8)
  if (-not (IsUserPtr $pawn)) { continue }   # 跳过垃圾槽（未填充/已释放）
  $hp    = ReadF32 ($pawn + 0x58)   # AttackableTarget.当前血量 (float)
  $maxHp = ReadU32 ($pawn + 0x5C)   # AttackableTarget.最大血量 (int)
  $shield= ReadU32 ($pawn + 0x60)   # AttackableTarget.护盾 (int)
  $alive = ReadU32 ($pawn + 0x3F8)  # Pawn.存活 (bool, 按 4B 读)
  $pinfo = ReadU64 ($pawn + 0x418)  # Pawn.PlayerInfo*
  $name = ""
  $camp = 0
  if ($pinfo -ne 0) {
    $namePtr = ReadU64 ($pinfo + 0x70)   # PlayerInfo.名字 String*
    if (IsUserPtr $namePtr) { $name = ReadWStr16 $namePtr 32 }
    $camp = ReadU32 ($pinfo + 0x138)     # PlayerInfo.队伍 ECamp
  }
  # CharacterController 活位置: pawn+0xC8(托管) -> +0x10(native) -> +0x0D4
  $ccMan = ReadU64 ($pawn + 0xC8)
  $cx = $cy = $cz = 0.0
  if ($ccMan -ne 0) {
    $cc = ReadU64 ($ccMan + 0x10)
    if ($cc -ne 0) {
      $cx = ReadF32 ($cc + 0x0D4); $cy = ReadF32 ($cc + 0x0D8); $cz = ReadF32 ($cc + 0x0DC)
    }
  }
  Write-Host ("pawn[{0}] = 0x{1:X}  hp={2:F0}/{3}  shield={4}  alive={5}  camp={6}  pos=({7:F1},{8:F1},{9:F1})  name={10}" -f $k, $pawn, $hp, $maxHp, $shield, $alive, $camp, $cx, $cy, $cz, $name)
}

Write-Host "== done (read-only, no memory written) =="
