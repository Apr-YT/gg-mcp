/* test_loader.c — GG 核心对接（loader）的文件操作回归
 *
 * 为什么值得测：loader_load 会**改写用户的 GG 核心库**（备份 → 装桩 → 装 hook），
 * 备份或恢复逻辑一旦出错，就是把用户的 lib5.so 弄坏且无法还原。
 * 属于破坏性操作，比搜索/冻结更需要回归保护。
 *
 * 只能在 Linux/Android 跑：install_file 用 rename() 覆盖目标文件，
 * Windows 的 rename 不覆盖已存在文件（POSIX 才覆盖），所以 Windows 上
 * 「装桩 / 装 hook / 还原」这几步根本走不通。Windows 侧由 run_tests.bat 跳过。
 *
 * 覆盖：
 *   1. load：备份原库 / 装桩 / 装 hook / 写 lib5_hook.conf
 *   2. 重复 load：不覆盖已存在的 .orig（保证备份永远是原始文件）
 *   3. restore：还原原库 / 删除 .orig 与 libKernelGg.so
 *   4. 边界：无 lib5 → NOT_FOUND；assets 缺桩 → INTERNAL；版本族不符 + strict → VERSION_MISMATCH
 */
#include "../../agentd/loader.h"
#include "../../shared/wire_protocol.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int g_fail = 0;

static void check(int cond, const char *what) {
    if (cond) {
        printf("ok  : %s\n", what);
    } else {
        printf("FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---------- 文件辅助 ---------- */

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("  [setup] 无法写入 %s\n", path);
        return;
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static int read_file(const char *path, char *out, size_t outsz) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(out, 1, outsz - 1, f);
    out[n] = 0;
    fclose(f);
    return (int)n;
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long s = ftell(f);
    fclose(f);
    return s;
}

static int file_has(const char *path, const char *needle) {
    char buf[512];
    if (read_file(path, buf, sizeof(buf)) < 0) return 0;
    return strstr(buf, needle) != NULL;
}

/* 删除本测试创建的文件（不递归，避免误删） */
static void clean_dir(const char *dir) {
    static const char *names[] = {
        "lib5.so", "lib5.so.orig", "lib5.so.primary",
        "libKernelGg.so", "lib5_hook.conf", NULL
    };
    char p[512];
    for (int i = 0; names[i]; i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        remove(p);
    }
    rmdir(dir);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    char root[] = "/tmp/ggmcp_loader_XXXXXX";
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }

    char assets[256], gg[256], p[512];
    snprintf(assets, sizeof(assets), "%s/assets", root);
    snprintf(gg, sizeof(gg), "%s/gg", root);
    mkdir(assets, 0755);
    mkdir(gg, 0755);

    /* 资源目录：桩 + hook 库（内容任意，只验证搬运与落位） */
    snprintf(p, sizeof(p), "%s/lib5_stub", assets);
    write_file(p, "STUB-BINARY");
    snprintf(p, sizeof(p), "%s/libKernelGg.so", assets);
    write_file(p, "HOOK-BINARY");

    /* GG 目录：原核心库 */
    char core[512], orig[512];
    snprintf(core, sizeof(core), "%s/lib5.so", gg);
    snprintf(orig, sizeof(orig), "%s/lib5.so.orig", gg);
    write_file(core, "ORIGINAL-GG-CORE");

    loader_set_assets_dir(assets);

    gg_ggdir_t g;
    memset(&g, 0, sizeof(g));
    snprintf(g.dir, sizeof(g.dir), "%s", gg);
    snprintf(g.pkg, sizeof(g.pkg), "com.tencent.gamereva");
    snprintf(g.version, sizeof(g.version), "101.1");

    printf("=== loader 文件操作 ===\n");

    /* ---- 1) 正常 load ---- */
    loader_set_hook_env("hook", NULL, NULL);
    int rc = loader_load(&g, "101.*", 1);
    check(rc == 0, "1) loader_load 返回 0");
    check(file_size(orig) > 0, "1) lib5.so.orig 已生成");
    check(file_has(orig, "ORIGINAL-GG-CORE"), "1) .orig 内容是原始库（不是桩）");
    check(file_has(core, "STUB-BINARY"), "1) lib5.so 已被替换为桩");
    snprintf(p, sizeof(p), "%s/libKernelGg.so", gg);
    check(file_has(p, "HOOK-BINARY"), "1) libKernelGg.so 已装到 GG 目录");
    snprintf(p, sizeof(p), "%s/lib5_hook.conf", gg);
    check(file_has(p, "AQ_KO_PROTO=hook"), "1) lib5_hook.conf 写入 AQ_KO_PROTO");
    check(g.stub_installed == 1, "1) stub_installed 标记置位");

    /* ---- 2) 重复 load 不覆盖已有备份 ---- */
    write_file(orig, "SENTINEL-BACKUP");
    rc = loader_load(&g, "101.*", 1);
    check(rc == 0, "2) 二次 loader_load 返回 0");
    check(file_has(orig, "SENTINEL-BACKUP"),
          "2) 已存在的 .orig 不被覆盖（备份始终是第一次的原件）");

    /* ---- 3) restore 还原 ---- */
    write_file(orig, "ORIGINAL-GG-CORE");   /* 还原刚才的哨兵 */
    rc = loader_restore(&g);
    check(rc == 0, "3) loader_restore 返回 0");
    check(file_has(core, "ORIGINAL-GG-CORE"), "3) lib5.so 已还原为原始内容");
    check(file_size(orig) < 0, "3) .orig 已删除");
    snprintf(p, sizeof(p), "%s/libKernelGg.so", gg);
    check(file_size(p) < 0, "3) GG 目录下的 libKernelGg.so 已删除");

    /* ---- 4) 边界 ---- */
    {
        gg_ggdir_t empty;
        memset(&empty, 0, sizeof(empty));
        snprintf(empty.dir, sizeof(empty.dir), "%s/nonexistent", root);
        check(loader_load(&empty, "101.*", 1) == -GGERR_NOT_FOUND,
              "4) 目录里没有 lib5 → NOT_FOUND");
    }
    {
        char empty_assets[512];
        snprintf(empty_assets, sizeof(empty_assets), "%s/empty_assets", root);
        mkdir(empty_assets, 0755);
        loader_set_assets_dir(empty_assets);
        write_file(core, "ORIGINAL-GG-CORE");
        check(loader_load(&g, "101.*", 1) == -GGERR_INTERNAL,
              "4) assets 缺桩/缺 hook → INTERNAL");
        loader_set_assets_dir(assets);
        rmdir(empty_assets);
    }
    {
        write_file(core, "ORIGINAL-GG-CORE");
        check(loader_load(&g, "999.*", 1) == -GGERR_VERSION_MISMATCH,
              "4) 版本族不符 + strict → VERSION_MISMATCH");
    }

    /* ---- 清理 ---- */
    clean_dir(gg);
    clean_dir(assets);
    rmdir(root);

    if (g_fail) {
        printf("\n== %d FAIL ==\n", g_fail);
        return 1;
    }
    printf("\n== ALL PASS ==\n");
    return 0;
}
