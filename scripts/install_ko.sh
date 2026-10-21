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

# 3) 逐个加载（按内核版本挑选 + 已加载跳过）
# 同一模块可能同时存在多份、分别对应不同内核（如 6.12.23 与 6.12.69）。
# 文件名规范：<模块名>_<x.y.z>.ko —— 只加载与当前内核完全一致的那份，
# 否则会把给别的机器编的模块 insmod 上去（vermagic 拒绝，或更糟：勉强加载后崩）。
KVER_SHORT=$(uname -r | sed -n 's/^\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p')
log "当前内核: ${KVER_SHORT:-unknown}（多版本 KO 只加载匹配的）"

for ko in $KOS; do
    name=$(basename "$ko" .ko)

    # 提取文件名末尾的 x.y.z；没有版本后缀则不筛选（兼容旧命名）
    kov=$(echo "$name" | sed -n 's/.*_\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)$/\1/p')
    if [ -n "$kov" ] && [ -n "$KVER_SHORT" ] && [ "$kov" != "$KVER_SHORT" ]; then
        log "跳过: $name（对应内核 $kov，当前 $KVER_SHORT）"
        continue
    fi

    if grep -q "^${name} " /proc/modules 2>/dev/null; then
        log "已加载: $name（跳过）"
        continue
    fi
    log "insmod $ko"
    err=$(insmod "$ko" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        log "加载成功: $name"
    elif echo "$err" | grep -qi "exists"; then
        # 模块开了 hide_by_default 时 /proc/modules 里看不到，重复 insmod 会返回 EEXIST。
        # 这不是失败，不能报成"加载失败"让人误以为驱动装不上。
        log "已加载: $name（模块已隐藏，跳过）"
    else
        log "加载失败: $name -> $err"
    fi
done

# 4) 恢复 SELinux enforcing
if [ "$SE_BEFORE" = "1" ]; then
    setenforce 1 2>/dev/null || log "setenforce 1 失败"
    log "SELinux 已恢复: $(getenforce 2>/dev/null)"
fi

# 5) 汇总
# 注意：模块开了 hide_by_default 后 /proc/modules 里看不到它们，
# 这里为空是正常的，判断是否成功要看上面的 insmod 结果。
log "== 相关模块（hide 后可能为空，属正常）=="
grep -iE "rwProc|hwBreakpoint|apr_dlkm" /proc/modules 2>/dev/null | tee -a "$LOG" || true

log "KO 加载完成。启动 agentd 后可用 gg_kernel_status 自检"
log "（hook 型期望 state=ko-hook；rwproc 型期望 state=kernel-driver）"
