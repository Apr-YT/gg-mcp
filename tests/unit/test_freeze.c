/* test_freeze.c — 冻结注册表列表管理单测（T3）
 * 覆盖 freeze_add（新增/同地址更新/超长拒绝/满额拒绝）/remove/clear/count。
 * 不启动循环线程（不触 driver_write），纯列表逻辑，MinGW 可编译。
 */
#include "../../agentd/freeze.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>

/* 循环线程引用 driver_write；测试不启动线程，提供 shim 满足链接 */
ssize_t driver_write(gg_driver_t *d, pid_t pid, uint64_t addr,
                     const void *buf, size_t len) {
    (void)d; (void)pid; (void)addr; (void)buf; (void)len;
    errno = ENOSYS;
    return -1;
}

/* freeze_loop 现在每轮比对 process_starttime 防 pid 复用；本测试不起线程，
 * 只需满足链接。返回 0 表示"进程不存在"——正好是本测试里 pid 的实际情况。 */
uint64_t process_starttime(pid_t pid) {
    (void)pid;
    return 0;
}

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("ok  : %s\n", msg); } \
    else { printf("[FAIL]: %s\n", msg); failures++; } \
} while (0)

int main(void) {
    gg_freeze_t f;
    CHECK(freeze_init(&f, NULL) == 0, "freeze_init 成功");

    /* 1) 新增单条 */
    uint32_t v1 = 0x12345678;
    CHECK(freeze_add(&f, 0x1000, &v1, sizeof(v1)) == 0, "1) add 单条返回 0");
    CHECK(freeze_count(&f) == 1, "1) count=1");
    CHECK(f.entries[0].addr == 0x1000 && f.entries[0].size == 4 &&
          memcmp(f.entries[0].value, &v1, 4) == 0, "1) 条目内容正确");
    CHECK(f.entries[0].active == 1, "1) active=1");

    /* 2) 同地址更新（不新增） */
    uint32_t v2 = 0xdeadbeef;
    CHECK(freeze_add(&f, 0x1000, &v2, sizeof(v2)) == 0, "2) add 同地址返回 0");
    CHECK(freeze_count(&f) == 1, "2) count 仍=1");
    CHECK(memcmp(f.entries[0].value, &v2, 4) == 0, "2) 值已更新");

    /* 3) 超长拒绝 */
    uint8_t big[9] = {0};
    CHECK(freeze_add(&f, 0x2000, big, 9) == -1, "3) size>8 拒绝");
    CHECK(freeze_count(&f) == 1, "3) count 不变");

    /* 4) 多条目 + remove */
    uint64_t v64 = 0x1122334455667788ull;
    CHECK(freeze_add(&f, 0x3000, &v64, 8) == 0, "4) add 8 字节条目");
    CHECK(freeze_count(&f) == 2, "4) count=2");
    CHECK(freeze_remove(&f, 0x1000) == 0, "4) remove 已存在返回 0");
    CHECK(freeze_count(&f) == 1, "4) count=1");
    CHECK(f.entries[0].addr == 0x3000, "4) 剩余条目是 0x3000（memmove 正确）");
    CHECK(freeze_remove(&f, 0x9999) == -1, "4) remove 不存在返回 -1");

    /* 5) clear */
    freeze_clear(&f);
    CHECK(freeze_count(&f) == 0, "5) clear 后 count=0");

    /* 6) 满额拒绝（GG_FREEZE_MAX=256） */
    for (size_t i = 0; i < GG_FREEZE_MAX; i++) {
        uint64_t a = 0x1000000ull + (uint64_t)i * 8;
        CHECK(freeze_add(&f, a, &v64, 8) == 0, "6) 填充条目");
        if (failures > 100) break;
    }
    CHECK(freeze_count(&f) == GG_FREEZE_MAX, "6) 满 256");
    CHECK(freeze_add(&f, 0x99999999, &v64, 8) == -1, "6) 第 257 条拒绝");

    /* 7) size=0 边界（清空后；add 空值不写内容但占位） */
    freeze_clear(&f);
    CHECK(freeze_count(&f) == 0, "7) clear 后 count=0");
    CHECK(freeze_add(&f, 0x7777, NULL, 0) == 0, "7) size=0 允许");
    CHECK(f.entries[f.count - 1].size == 0, "7) size 记录为 0");

    if (failures == 0) {
        printf("\nALL FREEZE TESTS PASSED\n");
        return 0;
    }
    printf("\n%d FAILURES\n", failures);
    return 1;
}
