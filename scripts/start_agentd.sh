#!/system/bin/sh
# start_agentd.sh — 设备端启动/停止 gg-agentd（KO 驱动方案）
# 用法: su -c 'sh /data/local/tmp/ggmcp/start_agentd.sh [start|stop|status|autostart]'
DIR=/data/local/tmp/ggmcp
BIN=$DIR/gg-agentd
PORT=19100
LOG=$DIR/gg-agentd.log

# KO 配置（可在此调整）
KO_DIR=$DIR/ko
KO_PATTERN=6.12.*
KO_STRICT=1
# 后端类型: hook | dev | qx | rwproc
#   hook  : 内核 hook process_vm_readv（零协议）
#   dev   : /dev 节点 + ioctl 0x7e1a/0x7e1b（通用布局, 填 DEV_PATH）
#   qx    : Aloha 开源 QX9-10（隐藏进程自动发现 + 建节点, 填 QX_KEY）
#   rwproc: abcz316/rwProcMem33（proc read 33B 头协议, 开源; 填 RWPROC_MEM_KEY/BP_KEY）
KO_TYPE=rwproc
DEV_PATH=
QX_KEY=
# rwProcMem33 两模块的 proc 节点 auth key（与编译模块时的 CONFIG_PROC_NODE_AUTH_KEY 一致;
# 留空则 agentd 自动扫描 /proc, 但节点被隐藏时扫描不到, 建议显式填写）
RWPROC_MEM_KEY=e84523d7b60d5d341a7c4d1861773ecd
RWPROC_BP_KEY=dce3771681d4c7a143d5d06b7d32548e
# 若用 dev 型, 填节点路径: DEV_PATH=/dev/your_ko_dev

# Magisk service.d 自启动: autostart 把本脚本 cp 进 service.d, Magisk 执行时 $1 为空,
# 走 * 分支只会打印用法不启动。这里自判 $0 含 service.d 且无参 → 自动 start。
case "$0" in
  */service.d/*) [ -z "$1" ] && set -- start ;;
esac

case "$1" in
  start)
    [ "$(id -u)" = "0" ] || { echo "需要 root"; exit 1; }
    [ -x "$BIN" ] || { echo "[FAIL] $BIN 不存在或不可执行 (adb push 会重置权限, 先 chmod 755)"; exit 1; }
    if pgrep -x gg-agentd >/dev/null 2>&1; then echo "gg-agentd 已在运行"; exit 0; fi
    ARGS="--port $PORT --driver $KO_TYPE"
    if [ "$KO_TYPE" = "qx" ]; then
        [ -n "$QX_KEY" ] && ARGS="$ARGS --qx-key $QX_KEY"
    elif [ "$KO_TYPE" = "rwproc" ]; then
        [ -n "$RWPROC_MEM_KEY" ] && ARGS="$ARGS --rwproc-key $RWPROC_MEM_KEY"
        [ -n "$RWPROC_BP_KEY" ] && ARGS="$ARGS --rwproc-bp-key $RWPROC_BP_KEY"
    else
        ARGS="$ARGS --ko-dir $KO_DIR --ko-pattern $KO_PATTERN --ko-strict $KO_STRICT"
        [ -n "$DEV_PATH" ] && ARGS="$ARGS --dev $DEV_PATH"
    fi
    setsid nohup "$BIN" $ARGS --log "$LOG" >/dev/null 2>&1 < /dev/null &
    # 轮询就绪(最多 5s, 每 0.5s), 避免 KO 加载慢时过早判失败
    i=0
    while [ $i -lt 10 ]; do
        if pgrep -x gg-agentd >/dev/null 2>&1; then
            echo "gg-agentd 已启动 (port $PORT, driver=$KO_TYPE, log $LOG)"
            exit 0
        fi
        sleep 0.5
        i=$((i+1))
    done
    echo "[FAIL] gg-agentd 启动失败(5s 内未见到进程), 检查日志: $LOG"
    exit 1
    ;;
  stop)
    pkill -x gg-agentd && echo "已停止" || echo "未在运行"
    # 等待进程真正退出, 防 deploy 连续 stop→start 时 pgrep 时序误判"已在运行"跳过启动
    i=0
    while pgrep -x gg-agentd >/dev/null 2>&1 && [ $i -lt 20 ]; do sleep 0.2; i=$((i+1)); done
    ;;
  status)
    if pgrep -x gg-agentd >/dev/null 2>&1; then
      echo "运行中:"; ps -A | grep gg-agentd
    else
      echo "未运行"
    fi
    ;;
  autostart)
    cp "$0" /data/adb/service.d/ggmcp-agentd.sh
    chmod 755 /data/adb/service.d/ggmcp-agentd.sh
    echo "已写入 /data/adb/service.d/ggmcp-agentd.sh"
    ;;
  *)
    echo "用法: $0 [start|stop|status|autostart]"
    ;;
esac
