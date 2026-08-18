#!/system/bin/sh
# install_ko.sh — 加载 KO 内核驱动（外挂圈子方式：insmod，不依赖 KPM/KPatch-Next）
# 用法: su -c 'sh /data/adb/GG-Ghost/install_ko.sh'
#
# 说明：
#   - KO 驱动文件放入 /data/adb/GG-Ghost/ko/ 目录
#   - 文件名可带版本号（如 driverQX114_6.12.9.ko），agentd 会按版本族模式
#     （默认 6.12.*）选择加载，同族补丁号任意均可加载
#   - SELinux：仅加载期间临时 permissive，完成后立即恢复 enforcing
#     （长期 permissive 是反作弊最高优先级检测点）
#   - dev 型 KO 的设备节点由 agentd 启动时按 /proc/misc 自动创建
set -e

DIR=/data/adb/GG-Ghost
KO_DIR=$DIR/ko
LOG=$DIR/install_ko.log

log() { echo "[install_ko] $1" | tee -a "$LOG"; }

log "== 环境检查 =="
[ "$(id -u)" = "0" ] || { log "需要 root (su) 运行"; exit 1; }
log "内核: $(uname -r)"
log "SELinux: $(getenforce 2>/dev/null || echo unknown)"

# 1) SELinux 临时 permissive（社区 KO 普遍需要），完成后恢复 enforcing
SE_BEFORE=$(cat /sys/fs/selinux/enforce 2>/dev/null || echo 0)
if [ "$SE_BEFORE" = "1" ]; then
    setenforce 0 2>/dev/null || log "setenforce 0 失败（可能已 permissive）"
    log "SELinux 已临时设为: $(getenforce 2>/dev/null)"
fi

# 2) 检查 KO 文件
KOS=$(ls "$KO_DIR"/*.ko 2>/dev/null || true)
if [ -z "$KOS" ]; then
    log "ko/ 目录下没有 .ko 文件。请把 KO 驱动（如 driverQX114_6.12.9.ko）放入 $KO_DIR/"
    exit 2
fi

# 3) 逐个加载（已加载的跳过）
for ko in $KOS; do
    name=$(basename "$ko" .ko)
    if grep -q "^${name} " /proc/modules 2>/dev/null; then
        log "已加载: $name（跳过）"
        continue
    fi
    log "insmod $ko"
    if insmod "$ko" 2>>"$LOG"; then
        log "加载成功: $name"
    else
        log "加载失败: $name（内核版本不匹配？SELinux？权限？）"
    fi
done

# 4) 恢复 SELinux enforcing
if [ "$SE_BEFORE" = "1" ]; then
    setenforce 1 2>/dev/null || log "setenforce 1 失败"
    log "SELinux 已恢复: $(getenforce 2>/dev/null)"
fi

# 5) 汇总
log "== /proc/modules =="
grep -E "$(ls "$KO_DIR" 2>/dev/null | sed 's/\.ko$//' | tr '\n' '|' | sed 's/|$//')" /proc/modules 2>/dev/null | tee -a "$LOG" || true

log "KO 加载完成。启动 agentd 后可用 gg_kernel_status 自检"
log "（hook 型期望 state=ko-hook；dev 型期望 state=kernel-driver）"
