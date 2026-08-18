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
# 后端类型: hook | dev | qx
#   hook: 内核 hook process_vm_readv（零协议）
#   dev : /dev 节点 + ioctl 0x7e1a/0x7e1b（通用布局, 填 DEV_PATH）
#   qx  : Aloha 开源 QX9-10（隐藏进程自动发现 + 建节点, 填 QX_KEY）
KO_TYPE=qx
DEV_PATH=
QX_KEY=
# 若用 dev 型, 填节点路径: DEV_PATH=/dev/your_ko_dev

case "$1" in
  start)
    [ "$(id -u)" = "0" ] || { echo "需要 root"; exit 1; }
    [ -x "$BIN" ] || { echo "[FAIL] $BIN 不存在或不可执行 (adb push 会重置权限, 先 chmod 755)"; exit 1; }
    if pgrep -f gg-agentd >/dev/null 2>&1; then echo "gg-agentd 已在运行"; exit 0; fi
    ARGS="--port $PORT --driver $KO_TYPE"
    if [ "$KO_TYPE" = "qx" ]; then
        [ -n "$QX_KEY" ] && ARGS="$ARGS --qx-key $QX_KEY"
    else
        ARGS="$ARGS --ko-dir $KO_DIR --ko-pattern $KO_PATTERN --ko-strict $KO_STRICT"
        [ -n "$DEV_PATH" ] && ARGS="$ARGS --dev $DEV_PATH"
    fi
    setsid nohup "$BIN" $ARGS --log "$LOG" >/dev/null 2>&1 < /dev/null &
    sleep 1
    if pgrep -f gg-agentd >/dev/null 2>&1; then
        echo "gg-agentd 已启动 (port $PORT, driver=$KO_TYPE, log $LOG)"
    else
        echo "[FAIL] gg-agentd 启动失败, 检查日志: $LOG"
        exit 1
    fi
    ;;
  stop)
    pkill -f gg-agentd && echo "已停止" || echo "未在运行"
    ;;
  status)
    if pgrep -f gg-agentd >/dev/null 2>&1; then
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
