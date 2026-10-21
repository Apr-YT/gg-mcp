#!/system/bin/sh
# start_agentd.sh — 设备端启动/停止 agentd（KO 驱动方案）
# 用法: su -c 'sh /data/adb/GG-Ghost/start_agentd.sh [start|stop|status|autostart]'
#
# 2026-08-18 hide 改造:
#   - 目录 /data/local/tmp/ggmcp → /data/adb/GG-Ghost（避开 ACE 重点扫描目录）
#   - 端口 19100 → 34127（非常见高位）
#   - 进程名伪装 lmkd（agentd 内部 prctl + 清 argv），pgrep/pkill 按名失效
#     → 改用 PID 文件管理
# 部署目录必须与 scripts/deploy.ps1 的 $DevDir 一致（/data/adb/GG-Ghost），
# 二进制在设备上伪装成 gdm。此前这里写的是 vendor_security/dspbridge，
# 与 deploy 脚本推的位置完全不同 —— 表现为 stop/start 全部返回 1、agentd 起不来。
DIR=/data/adb/GG-Ghost
BIN=$DIR/gdm
PORT=34127
LOG=$DIR/agentd.log
PIDFILE=$DIR/agentd.pid

# 驱动配置
# 后端只有 rwproc33 一种（hook / dev / qx 三型已于 2026-10-21 删除）。
# KO 的 insmod 与版本选择由 install_ko.sh 负责，agentd 不参与加载。
KO_TYPE=rwproc
# 注意：硬件断点（gg_watch_*）默认关闭。当前 hwBreakpointProc prebuilt 是未修复版本，
# 实例化断点会让整机重启（证据见 docs/engineering/DRIVER_MATRIX.md）。
# 换成按 RWPROC33_ADAPTER.md 10.4 自编译的模块后，才在 ARGS 里加 --hwbp。
# rwProcMem33 两模块的 proc 节点 auth key（与编译模块时的 CONFIG_PROC_NODE_AUTH_KEY 一致;
# 留空则 agentd 自动扫描 /proc, 但节点被隐藏时扫描不到, 建议显式填写）
RWPROC_MEM_KEY=e84523d7b60d5d341a7c4d1861773ecd
RWPROC_BP_KEY=dce3771681d4c7a143d5d06b7d32548e

# wire 握手密钥文件（0600, 仅 root 可读）。存在即启用鉴权：
# 设备上普通 App 即使能连 127.0.0.1:$PORT，没有密钥也无法调用任何 root 级命令。
# 首次启动自动生成；PC 端需配置同一值（deploy.ps1 会自动拉取并写入 configs）。
WIRE_KEY_FILE=$DIR/wire.key

is_running() {
    [ -f "$PIDFILE" ] || return 1
    local pid; pid=$(cat "$PIDFILE" 2>/dev/null)
    [ -n "$pid" ] || return 1
    kill -0 "$pid" 2>/dev/null
}

# Magisk service.d 自启动: autostart 把本脚本 cp 进 service.d, Magisk 执行时 $1 为空,
# 走 * 分支只会打印用法不启动。这里自判 $0 含 service.d 且无参 → 自动 start。
case "$0" in
  */service.d/*) [ -z "$1" ] && set -- start ;;
esac

case "$1" in
  start)
    [ "$(id -u)" = "0" ] || { echo "需要 root"; exit 1; }
    [ -x "$BIN" ] || { echo "[FAIL] $BIN 不存在或不可执行 (adb push 会重置权限, 先 chmod 755)"; exit 1; }
    if is_running; then echo "agentd 已在运行 (pid $(cat $PIDFILE))"; exit 0; fi
    ARGS="--port $PORT --driver $KO_TYPE"
    # wire 握手密钥：文件不存在则生成 32 字节随机 hex（0600，root only）
    if [ ! -s "$WIRE_KEY_FILE" ]; then
        mkdir -p "$DIR" 2>/dev/null
        head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$WIRE_KEY_FILE"
        chmod 600 "$WIRE_KEY_FILE"
    fi
    if [ -s "$WIRE_KEY_FILE" ]; then
        ARGS="$ARGS --wire-key-file $WIRE_KEY_FILE"
    else
        echo "[WARN] $WIRE_KEY_FILE 不可用, agentd 将以无鉴权模式启动"
    fi
    # 只传 rwproc 的参数。agentd 对未知参数是 "unknown arg" + return 1（直接退出），
    # 所以绝不能再传 --dev / --qx-key 这类已删除的历史参数。
    [ -n "$RWPROC_MEM_KEY" ] && ARGS="$ARGS --rwproc-key $RWPROC_MEM_KEY"
    [ -n "$RWPROC_BP_KEY" ] && ARGS="$ARGS --rwproc-bp-key $RWPROC_BP_KEY"
    setsid nohup "$BIN" $ARGS --log "$LOG" >/dev/null 2>&1 < /dev/null &
    echo $! > "$PIDFILE"
    # 轮询就绪(最多 5s, 每 0.5s), 避免 KO 加载慢时过早判失败
    i=0
    while [ $i -lt 10 ]; do
        if is_running; then
            echo "agentd 已启动 (port $PORT, driver=$KO_TYPE, log $LOG)"
            exit 0
        fi
        sleep 0.5
        i=$((i+1))
    done
    echo "[FAIL] agentd 启动失败(5s 内未就绪), 检查日志: $LOG"
    exit 1
    ;;
  stop)
    if is_running; then
        kill "$(cat $PIDFILE)" && echo "已停止" || echo "kill 失败"
        rm -f "$PIDFILE"
    else
        echo "未在运行"; rm -f "$PIDFILE"
    fi
    # 等待进程真正退出, 防 deploy 连续 stop→start 时 PID 文件时序误判
    i=0
    while is_running && [ $i -lt 20 ]; do sleep 0.2; i=$((i+1)); done
    ;;
  status)
    if is_running; then
      echo "运行中: pid $(cat $PIDFILE)"; ps -A | grep -E "lmkd|$(cat $PIDFILE)" | head -3
    else
      echo "未运行"
    fi
    ;;
  autostart)
    # Magisk service.d（Magisk 设备用）
    cp "$0" /data/adb/service.d/90gdm.sh
    chmod 755 /data/adb/service.d/90gdm.sh
    echo "已写入 /data/adb/service.d/90gdm.sh"
    ;;
  autostart-ksu)
    # KernelSU 模块 boot service（KernelSU 无 service.d, 用 /data/adb/modules/）
    mkdir -p /data/adb/modules/vsec
    cat > /data/adb/modules/vsec/module.prop <<'KEOF'
id=vsec
name=vsec-autostart
version=1
versionCode=1
author=gg
description=autostart dspbridge + apr_dlkm at boot
KEOF
    cat > /data/adb/modules/vsec/service.sh <<'KEOF'
#!/system/bin/sh
# KernelSU boot: 启动 agentd（dspbridge 伪装）+ 内核通道恢复
sh /data/adb/GG-Ghost/start_agentd.sh start
KEOF
    chmod 755 /data/adb/modules/vsec/service.sh
    echo "已写入 /data/adb/modules/vsec (KernelSU boot service)"
    echo "注意: KernelSU 需在管理器里对 vsec 模块保持启用, 重启后 service.sh 自动执行"
    ;;
  *)
    echo "用法: $0 [start|stop|status|autostart|autostart-ksu]"
    ;;
esac
