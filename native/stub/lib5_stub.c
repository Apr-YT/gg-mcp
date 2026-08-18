/* lib5_stub.c — GG 核心 ELF 桩加载器（静态 PIE）
 *
 * 背景：GG 守护链为 GG主程序 → su → lib05.so(守护) → exec <ggdir>/lib5.so。
 * 守护进程要求 lib5.so 必须是合法 ELF（脚本方案触发 "already run"）。
 *
 * 方案：本桩编译为静态 PIE ELF，替换 <ggdir>/lib5.so；
 *       原库备份为 lib5.so.orig。桩运行时：
 *         1) 读 /proc/self/exe 得自身路径
 *         2) 目标 = 自身路径 + ".orig"，hook = 同目录 libKernelGg.so
 *         3) 若存在同目录 lib5_hook.conf（loader 写入）→ 逐行 setenv AQ_KO_*（驱动协议）
 *         4) setenv LD_PRELOAD=<hook> 后 execv 原库
 *   由此保证 LD_PRELOAD 先于 lib5.so 加载（RTLD_NEXT 作用域要求）。
 *
 * 编译: aarch64-linux-androidXX-clang -static-pie -fPIE -O2 -o lib5_stub lib5_stub.c
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>

/* 从 <dir>/lib5_hook.conf 读取 AQ_KO_* 环境变量（驱动协议透传给 hook） */
static void load_hook_conf(const char *dir) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/lib5_hook.conf", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (len == 0 || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        if (strncmp(line, "AQ_KO_", 6) == 0) {
            setenv(line, eq + 1, 1);
        }
    }
    fclose(f);
}

int main(int argc, char **argv) {
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        fprintf(stderr, "lib5_stub: cannot resolve /proc/self/exe\n");
        return 127;
    }
    self[n] = 0;

    /* 目录与原始库路径 */
    char self_copy[PATH_MAX];
    snprintf(self_copy, sizeof(self_copy), "%s", self);
    char *dir = dirname(self_copy);

    char orig[PATH_MAX], hook[PATH_MAX];
    snprintf(orig, sizeof(orig), "%s.orig", self);
    snprintf(hook, sizeof(hook), "%s/libKernelGg.so", dir);

    if (access(orig, X_OK) != 0) {
        fprintf(stderr, "lib5_stub: original library missing: %s\n", orig);
        return 126;
    }
    if (access(hook, R_OK) != 0) {
        fprintf(stderr, "lib5_stub: hook library missing: %s\n", hook);
        return 125;
    }

    /* 驱动协议环境（qx/dev/hook） */
    load_hook_conf(dir);
    /* 先于目标库加载 hook */
    setenv("LD_PRELOAD", hook, 1);

    execv(orig, argv);
    fprintf(stderr, "lib5_stub: exec failed: %s\n", orig);
    return 124;
}
