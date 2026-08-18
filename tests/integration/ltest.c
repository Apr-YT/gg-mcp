/* ltest.c — loader_find_dirs 独立测试（诊断用）
 * 编译: aarch64-linux-android28-clang -O2 -I<root> ltest.c agentd/loader.c agentd/version.c third_party/cJSON.c -pie
 * 运行: adb shell su -c '/data/local/tmp/ggmcp/ltest'
 */
#include "../../agentd/loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== ltest: loader_find_dirs ==\n");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    size_t n = 0;
    gg_ggdir_t *dirs = loader_find_dirs(&n, "101.*");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("loader_find_dirs 耗时: %.0f ms, 找到 %zu 个目录\n", ms, n);
    for (size_t i = 0; i < n; i++) {
        printf("  [%zu] dir=%s pkg=%s version=%s has_lib5=%d match=%d stub=%d hook=%d\n",
               i, dirs[i].dir, dirs[i].pkg, dirs[i].version,
               dirs[i].has_lib5, dirs[i].family_match,
               dirs[i].stub_installed, dirs[i].hook_loaded);
    }
    free(dirs);
    return 0;
}
