/* ko_downgrade.h — KO 驱动降级决策状态机（平台无关，可单测）
 *
 * 背景（C5 修复）：
 *   libKernelGg.so 的 driver_io 通过 /dev 节点 ioctl 读写内核驱动。
 *   原实现：任意单次 ioctl 失败即把 s_tested=-1（永久降级），
 *   之后所有调用短路到真实 syscall。但 GG 扫描大进程时偶发不可读页
 *   (EFAULT/权限不足) 极其常见，会**误杀**正常可用的 KO 驱动。
 *
 * 设计：
 *   区分"单次请求失败"与"驱动整体不可用"。
 *   - 单次请求失败（如某地址不可读/权限不足）→ 仅本次回退真实 syscall，不降级。
 *   - 连续失败 >= 阈值 → 判驱动不可用，永久降级（短路，避免反复无效内核往返）。
 *   - 结构性失败（通道 open 失败 / init_key 失败）→ 直接永久降级。
 *
 * 该状态机被 hook.c 直接引用，单测见 tests/unit/test_ko_downgrade.c。
 */
#ifndef GG_KO_DOWNGRADE_H
#define GG_KO_DOWNGRADE_H

#ifndef GG_KO_FAIL_THRESHOLD
#define GG_KO_FAIL_THRESHOLD 32   /* 连续失败达此数才判驱动整体不可用 */
#endif

typedef struct ko_downgrade_state {
    int fail_streak;   /* 连续失败计数 */
    int downgraded;    /* 0=可用, 1=已永久降级（结构性，需重启进程才能恢复） */
} ko_downgrade_state_t;

/* 记录一次请求结果。
 *   ok=1（成功）→ 清零连续失败，不降级。
 *   ok=0（失败）→ 连续失败+1；达到阈值则置 downgraded=1。
 * 返回 1 表示当前应永久降级（短路），0 表示驱动仍可用。
 * 注意：成功也可能已处于 downgraded 态（结构性），维持不变。 */
static inline int ko_downgrade_record(ko_downgrade_state_t *s, int ok) {
    if (ok) {
        s->fail_streak = 0;
        return s->downgraded;
    }
    if (s->downgraded) return 1;
    if (++s->fail_streak >= GG_KO_FAIL_THRESHOLD) {
        s->downgraded = 1;
        return 1;
    }
    return 0;
}

/* 结构性失败（通道 open 失败 / init_key 失败）→ 直接永久降级 */
static inline void ko_downgrade_hard(ko_downgrade_state_t *s) {
    s->downgraded = 1;
    s->fail_streak = GG_KO_FAIL_THRESHOLD;
}

#endif /* GG_KO_DOWNGRADE_H */
