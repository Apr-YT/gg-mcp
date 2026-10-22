#!/system/bin/sh
# check_stealth.sh — 隐藏性自检（2026-10-23）
#
# 把目前已知的所有"能否发现驱动"的检测面逐条跑一遍，输出 PASS/FAIL。
# 改完驱动/agentd 后跑这个，比逐条手工敲快。
#
# 用法：adb push 到设备后
#   su -c 'sh /data/adb/GG-Ghost/check_stealth.sh'
#
# 退出码：有 FAIL 时为 1，便于脚本串联。

MEMKEY=${MEMKEY:-e84523d7b60d5d341a7c4d1861773ecd}
BPKEY=${BPKEY:-dce3771681d4c7a143d5d06b7d32548e}
NPASS=0
NFAIL=0

ok()  { echo "[PASS] $1"; NPASS=$((NPASS + 1)); }
bad() { echo "[FAIL] $1"; NFAIL=$((NFAIL + 1)); }

echo "=== GG 隐藏性自检 ==="

# 1. dmesg：模块的任何明文字符串都是指纹
n=$(dmesg 2>/dev/null | grep -icE 'rwproc|rwnova|rwproc33|dmabuf: init')
[ "$n" -eq 0 ] && ok "dmesg 无模块痕迹" || bad "dmesg 有 $n 条痕迹"

# 2. /proc/modules：list_del_init 后应查不到
n=$(grep -icE 'rwnova|rwprocmem' /proc/modules 2>/dev/null)
[ "$n" -eq 0 ] && ok "/proc/modules 不可见" || bad "/proc/modules 仍可见（$n 条）"

# 3. /sys/module：kobject_del 后应查不到
n=$(ls /sys/module 2>/dev/null | grep -icE 'rwnova|rwprocmem')
[ "$n" -eq 0 ] && ok "/sys/module 不可见" || bad "/sys/module 仍可见（$n 条）"

# 4. ls /proc：readdir 过滤（kprobe hook proc_root_readdir）
n=$(ls /proc 2>/dev/null | grep -cE "$MEMKEY|$BPKEY")
[ "$n" -eq 0 ] && ok "ls /proc 查不到节点目录" || bad "ls /proc 能看到节点目录"

# 5. 节点权限：必须 0600（root only）。0666 等于"任何 App 都能开"
perm=$(ls -l "/proc/$MEMKEY/$MEMKEY" 2>/dev/null | cut -d' ' -f1)
case "$perm" in
    -rw-------) ok "节点权限 0600（$perm）" ;;
    '')         bad "节点不存在（/proc/$MEMKEY/$MEMKEY）" ;;
    *)          bad "节点权限是 $perm，期望 -rw-------" ;;
esac

# 6. agentd 的 fd 表：proc 节点链接 = 扫 fd 表就能发现，不需要知道 key
pid=$(pidof gdm 2>/dev/null)
if [ -n "$pid" ]; then
    n=$(ls -l "/proc/$pid/fd" 2>/dev/null | grep -cE "/proc/$MEMKEY|/proc/$BPKEY")
    [ "$n" -eq 0 ] && ok "agentd fd 表无 proc 节点链接" \
                   || bad "agentd fd 表有 $n 个节点链接（扫 fd 表即可发现）"

    n=$(ls -l "/proc/$pid/fd" 2>/dev/null | grep -c 'anon_inode:dmabuf')
    [ "$n" -ge 2 ] && ok "匿名 fd 数 $n（期望 2：内存 + 断点）" \
                   || bad "匿名 fd 数 $n，期望 2（某模块没升级或已回退 proc）"

    n=$(ls -l "/proc/$pid/fd" 2>/dev/null | grep -c 'anon_inode:rwproc')
    [ "$n" -eq 0 ] && ok "无 anon_inode:rwproc 指纹" \
                   || bad "fd 名仍是 anon_inode:rwproc（应改成 dmabuf）"
else
    bad "agentd(gdm) 未运行，跳过 fd 表检查"
fi

# 7. kallsyms：模块符号不应出现在全局符号表里
n=$(grep -c '\[rwNova' /proc/kallsyms 2>/dev/null)
[ "$n" -eq 0 ] && ok "kallsyms 无模块符号" || bad "kallsyms 有 $n 个模块符号"

# 8. debugfs kprobe 表：hide_procfs_dir 用 kprobe hook proc_root_readdir
if [ -r /sys/kernel/debug/kprobes/list ]; then
    n=$(grep -c 'proc_root_readdir' /sys/kernel/debug/kprobes/list 2>/dev/null)
    [ "$n" -eq 0 ] && ok "kprobe 表无 proc_root_readdir 探针" \
                   || bad "kprobe 表暴露 proc_root_readdir（隐藏用的探针）"
else
    ok "debugfs 未挂载，kprobe 表不可读"
fi

# 9. 非 root（uid 2000 shell）能否打开节点
if command -v su >/dev/null 2>&1; then
    if su shell -c "head -c 1 /proc/$MEMKEY/$MEMKEY" >/dev/null 2>&1; then
        bad "非 root 能打开节点（权限没生效）"
    else
        ok "非 root 打开被拒绝"
    fi
fi

# 10. 模块是否真的可用（隐藏不能以功能为代价）
if [ -n "$pid" ]; then
    tail -c 2000 /data/adb/GG-Ghost/agentd.log 2>/dev/null | grep -q 'channel OK' \
        && ok "agentd self-test channel OK" \
        || bad "agentd 日志里没有 channel OK"
fi

echo "=== 结果：PASS=$NPASS FAIL=$NFAIL ==="
[ "$NFAIL" -eq 0 ] || exit 1
