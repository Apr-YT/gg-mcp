# build_apk.ps1 — 构建 GG-MCP APK（无 Gradle，纯命令行 aapt2/d8/apksigner）
# aapt2 不认中文路径 → 在 ASCII 暂存目录 D:\Kiln\apk-stage 构建，成品拷回 apk\build\
# 用法: powershell -ExecutionPolicy Bypass -File scripts\build_apk.ps1 [-Install]
param([switch]$Install)

$ErrorActionPreference = 'Stop'
$ROOT = Split-Path -Parent $PSScriptRoot
$APK = Join-Path $ROOT 'apk'
$STAGE = 'D:\Kiln\apk-stage'

# ---- 工具链路径 ----
$JDK = Get-ChildItem 'D:\Kiln\jdk17' -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $JDK) { throw 'JDK not found' }
$AAPT2 = Get-ChildItem 'D:\Kiln\sdk-dl\bt','D:\Kiln\android-sdk\build-tools' -Recurse -Filter 'aapt2.exe' -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
if (-not $AAPT2) { throw 'aapt2 not found' }
$BTDIR = Split-Path $AAPT2
$PLAT_JAR = Get-ChildItem 'D:\Kiln\sdk-dl\plat','D:\Kiln\android-sdk\platforms' -Recurse -Filter 'android.jar' -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
if (-not $PLAT_JAR) { throw 'android.jar not found' }

$JAVAC = Join-Path $JDK.FullName 'bin\javac.exe'
$JAR = Join-Path $JDK.FullName 'bin\jar.exe'
$KEYTOOL = Join-Path $JDK.FullName 'bin\keytool.exe'
$D8 = Join-Path $BTDIR 'd8.bat'
$ZIPALIGN = Join-Path $BTDIR 'zipalign.exe'
$APKSIGNER = Join-Path $BTDIR 'apksigner.bat'

# ---- 暂存（ASCII 路径）----
Remove-Item $STAGE -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $STAGE | Out-Null
Copy-Item -Recurse -Force (Join-Path $APK 'res') (Join-Path $STAGE 'res')
Copy-Item -Recurse -Force (Join-Path $APK 'java') (Join-Path $STAGE 'java')
Copy-Item -Force (Join-Path $APK 'AndroidManifest.xml') (Join-Path $STAGE 'AndroidManifest.xml')
New-Item -ItemType Directory -Force -Path (Join-Path $STAGE 'assets') | Out-Null
Copy-Item (Join-Path $ROOT 'build\agentd\gg-agentd') (Join-Path $STAGE 'assets\gg-agentd') -Force
Copy-Item (Join-Path $ROOT 'scripts\start_agentd.sh') (Join-Path $STAGE 'assets\start_agentd.sh') -Force

$B = Join-Path $STAGE 'build'
New-Item -ItemType Directory -Force -Path (Join-Path $B 'obj'),(Join-Path $B 'gen'),(Join-Path $B 'dex'),(Join-Path $B 'res') | Out-Null

# ---- 1) aapt2 ----
Write-Host '[1/6] aapt2 compile+link ...'
& $AAPT2 compile --dir (Join-Path $STAGE 'res') -o (Join-Path $B 'res\res.zip')
if ($LASTEXITCODE -ne 0) { throw 'aapt2 compile failed' }
& $AAPT2 link -I $PLAT_JAR `
    --manifest (Join-Path $STAGE 'AndroidManifest.xml') `
    -R (Join-Path $B 'res\res.zip') `
    --java (Join-Path $B 'gen') `
    -A (Join-Path $STAGE 'assets') `
    --auto-add-overlay `
    -o (Join-Path $B 'base.apk')
if ($LASTEXITCODE -ne 0) { throw 'aapt2 link failed' }

# ---- 2) javac ----
Write-Host '[2/6] javac ...'
$javaFiles = @(Get-ChildItem (Join-Path $B 'gen') -Recurse -Filter '*.java' | ForEach-Object { $_.FullName })
$javaFiles += @(Get-ChildItem (Join-Path $STAGE 'java') -Recurse -Filter '*.java' | ForEach-Object { $_.FullName })
& $JAVAC -encoding UTF-8 -source 8 -target 8 -classpath $PLAT_JAR -d (Join-Path $B 'obj') $javaFiles
if ($LASTEXITCODE -ne 0) { throw 'javac failed' }

# ---- 3) d8 ----
Write-Host '[3/6] d8 ...'
$env:JAVA_HOME = $JDK.FullName
$env:PATH = "$(Join-Path $JDK.FullName 'bin');" + $env:PATH
& $D8 --release --lib $PLAT_JAR --output (Join-Path $B 'dex') (Get-ChildItem (Join-Path $B 'obj') -Recurse -Filter '*.class' | ForEach-Object { $_.FullName })
if ($LASTEXITCODE -ne 0) { throw 'd8 failed' }

# ---- 4) classes.dex ----
Write-Host '[4/6] jar uf classes.dex ...'
Push-Location (Join-Path $B 'dex')
& $JAR uf (Join-Path $B 'base.apk') classes.dex
Pop-Location

# ---- 5) zipalign + sign ----
Write-Host '[5/6] zipalign + apksigner ...'
# keystore 持久化在项目 apk\ggmcp.keystore（签名一致性，可覆盖升级）
$KS = Join-Path $APK 'ggmcp.keystore'
if (-not (Test-Path $KS)) {
    $oldEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $KEYTOOL -genkeypair -keystore $KS -alias ggmcp -keyalg RSA -keysize 2048 -validity 10000 `
        -storepass ggmcp123 -keypass ggmcp123 -dname 'CN=ggmcp,OU=dev,O=ggmcp,L=city,ST=st,C=cn' 2>&1 | Out-Null
    $ErrorActionPreference = $oldEAP
}
& $ZIPALIGN -f 4 (Join-Path $B 'base.apk') (Join-Path $B 'aligned.apk')
& $APKSIGNER sign --ks $KS --ks-pass pass:ggmcp123 --key-pass pass:ggmcp123 --out (Join-Path $B 'ggmcp.apk') (Join-Path $B 'aligned.apk')
if ($LASTEXITCODE -ne 0) { throw 'apksigner failed' }

# ---- 拷贝成品回项目 ----
New-Item -ItemType Directory -Force -Path (Join-Path $APK 'build') | Out-Null
Copy-Item (Join-Path $B 'ggmcp.apk') (Join-Path $APK 'build\ggmcp.apk') -Force
$OUT = Join-Path $APK 'build\ggmcp.apk'
Write-Host "[OK] $OUT  size=$((Get-Item $OUT).Length)"

if ($Install) {
    Write-Host '[6/6] adb install ...'
    & 'D:\Kiln\ADB\adb.exe' install -r $OUT
}
