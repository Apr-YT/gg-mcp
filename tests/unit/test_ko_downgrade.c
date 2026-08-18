/* test_ko_downgrade.c — ko_downgrade 状态机单元测试（MinGW/x86 可编译，平台无关）
 *
 * 守护 C5 修复：
 *   单次请求失败不永久降级；仅连续失败达阈值才降级；
 *   结构性失败（init_key/open）直接降级且不可逆。
 * 编译: gcc -I../native/hook test_ko_downgrade.c -o test_ko_downgrade
 */
#include <stdio.h>
#include "ko_downgrade.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); failures++; } \
} while (0)

int main(void) {
    /* 1. 初始状态可用 */
    ko_downgrade_state_t s = {0, 0};
    CHECK(s.downgraded == 0);
    CHECK(s.fail_streak == 0);

    /* 2. 第 1 次失败不降级，仅计数 */
    CHECK(ko_downgrade_record(&s, 0) == 0);
    CHECK(s.downgraded == 0);
    CHECK(s.fail_streak == 1);

    /* 3. 偶发失败（交替 失败/成功）永不累积降级：模拟扫描中零星不可读页 */
    for (int i = 0; i < 1000; i++) {
        ko_downgrade_record(&s, 0);   /* 单次失败 */
        ko_downgrade_record(&s, 1);   /* 成功重置 */
    }
    CHECK(s.downgraded == 0);
    CHECK(s.fail_streak == 0);

    /* 4. 连续失败达阈值才降级 */
    ko_downgrade_state_t s2 = {0, 0};
    int last = 0;
    for (int i = 1; i <= GG_KO_FAIL_THRESHOLD; i++) {
        last = ko_downgrade_record(&s2, 0);
        if (i < GG_KO_FAIL_THRESHOLD) CHECK(last == 0);  /* 阈值前不降级 */
    }
    CHECK(last == 1);                  /* 第阈值次 → 降级 */
    CHECK(s2.downgraded == 1);
    CHECK(s2.fail_streak == GG_KO_FAIL_THRESHOLD);

    /* 5. 降级后即使偶发成功也维持降级（结构性判定不可逆，需重启进程恢复） */
    CHECK(ko_downgrade_record(&s2, 1) == 1);
    CHECK(s2.downgraded == 1);

    /* 6. 结构性失败直接降级 */
    ko_downgrade_state_t s3 = {0, 0};
    ko_downgrade_hard(&s3);
    CHECK(s3.downgraded == 1);
    CHECK(ko_downgrade_record(&s3, 0) == 1);  /* 降级态下任何失败都短路 */

    /* 7. 阈值边界：恰好阈值-1 次失败不降级 */
    ko_downgrade_state_t s4 = {0, 0};
    for (int i = 0; i < GG_KO_FAIL_THRESHOLD - 1; i++) {
        CHECK(ko_downgrade_record(&s4, 0) == 0);
    }
    CHECK(s4.downgraded == 0);
    ko_downgrade_record(&s4, 1);       /* 重置 */
    CHECK(s4.fail_streak == 0);

    if (failures == 0) {
        printf("ALL KO DOWNGRADE TESTS PASSED (%d cases)\n", 7);
        return 0;
    }
    fprintf(stderr, "%d CHECK(s) FAILED\n", failures);
    return 1;
}
