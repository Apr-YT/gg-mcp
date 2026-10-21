/* loader.c — GG 核心对接编排实现 */
#include "loader.h"
#include "version.h"
#include "../shared/wire_protocol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* 桩与 hook 库存放处。必须与 deploy.ps1 的 $DevDir / start_agentd.sh 的 DIR 一致：
 * 三处原本各不相同（vendor_security vs GG-Ghost vs local/tmp/ggmcp），
 * 导致 loader_load 找不到 lib5_stub 而返回 GGERR_INTERNAL，GG 核心加载必然失败。
 * 允许 -D 覆盖，便于单测指向临时目录。 */
#ifndef AGENTD_ASSETS_DIR
#define AGENTD_ASSETS_DIR "/data/adb/GG-Ghost"
#endif

/* 资源目录（桩与 hook 库所在）。默认是编译期常量；
 * loader_set_assets_dir 仅供单元测试指向临时目录，运行时路径不要调用。 */
static const char *g_assets_dir = AGENTD_ASSETS_DIR;

void loader_set_assets_dir(const char *dir) {
    if (dir && *dir) g_assets_dir = dir;
}

/* hook 驱动协议环境（由 main 在驱动对接后设置，loader_load 写入 lib5_hook.conf） */
static char g_hook_proto[16] = "hook";
static char g_hook_dev[128] = "";
static char g_hook_key[256] = "";

void loader_set_hook_env(const char *proto, const char *dev, const char *key) {
    if (proto) snprintf(g_hook_proto, sizeof(g_hook_proto), "%s", proto);
    if (dev) snprintf(g_hook_dev, sizeof(g_hook_dev), "%s", dev);
    if (key) snprintf(g_hook_key, sizeof(g_hook_key), "%s", key);
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int copy_file(const char *src, const char *dst, mode_t mode) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t w = write(out, buf, (size_t)n);
        if (w != n) { close(in); close(out); return -1; }
    }
    close(in);
    close(out);
    chmod(dst, mode);
    return 0;
}

/* 原子安装: 复制到 <dst>.tmp 后 rename 覆盖。
 * ⚠️ 不能直接 open(dst, O_WRONLY|O_TRUNC)：目标若正被进程 exec 运行
 * （GG 核心 lib5.so），内核返回 ETXTBSY ("Text file busy")；
 * rename 覆盖运行中的可执行文件是允许的，且原子。 */
static int install_file(const char *src, const char *dst, mode_t mode) {
    char tmp[GG_DIR_MAX + 64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    remove(tmp);
    if (copy_file(src, tmp, mode) < 0) return -1;
    if (rename(tmp, dst) != 0) { remove(tmp); return -1; }
    return 0;
}

/* 提取 <ggdir> 下的核心库文件名: 优先 lib5.so / lib5.so.primary（快速路径） */
static const char *find_lib5_known(const char *dir) {
    static char buf[GG_DIR_MAX + 32];
    snprintf(buf, sizeof(buf), "%s/lib5.so", dir);
    if (file_exists(buf)) return "lib5.so";
    snprintf(buf, sizeof(buf), "%s/lib5.so.primary", dir);
    if (file_exists(buf)) return "lib5.so.primary";
    return NULL;
}

/* 完整路径: 已知名 + 兜底扫描目录内最大的 AArch64/ARM 可执行 ELF。
 * 仅对疑似 GG 目录（GG- 前缀）使用, 避免对普通应用的海量文件做 I/O 风暴。 */
static const char *find_lib5(const char *dir) {
    const char *known = find_lib5_known(dir);
    if (known) return known;

    /* 兜底: 扫描目录内最大的 AArch64/ARM 可执行 ELF（GG 核心通常是最大者,
     * 支持库仅几十~几百 KB） */
    DIR *dp = opendir(dir);
    if (!dp) return NULL;
    struct dirent *de;
    static char chosen[GG_DIR_MAX + 32];
    chosen[0] = 0;
    long best_size = 0;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char p[GG_DIR_MAX + 32];
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        unsigned char hdr[20];
        size_t rd = fread(hdr, 1, sizeof(hdr), f);
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        fclose(f);
        if (rd < 20) continue;
        /* ELF + ET_DYN(0x03) + AArch64(0xB7)/ARM(0x28) */
        if (hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F' &&
            hdr[16] == 3 && hdr[17] == 0 && (hdr[18] == 0xB7 || hdr[18] == 0x28)) {
            if (fsz > best_size) {
                best_size = fsz;
                snprintf(chosen, sizeof(chosen), "%s", de->d_name);
            }
        }
    }
    closedir(dp);
    if (chosen[0]) return chosen;
    return NULL;
}

/* 读取 <ggdir>/version.gg（格式 "versionCode:versionName"，如 "16142:101.1"）。
 * 成功返回 0 并填充 out（malloc）；失败返回 -1。 */
static int version_from_gg_file(const char *dir, char **out) {
    char path[GG_DIR_MAX + 32];
    snprintf(path, sizeof(path), "%s/version.gg", dir);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[128];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return -1;
    buf[n] = 0;
    /* 取冒号后的部分（versionName） */
    char *colon = strchr(buf, ':');
    char *ver = colon ? colon + 1 : buf;
    char *nl = strchr(ver, '\n');
    if (nl) *nl = 0;
    while (*ver == ' ' || *ver == '\t') ver++;
    size_t len = strlen(ver);
    while (len > 0 && (ver[len - 1] == ' ' || ver[len - 1] == '\r')) ver[--len] = 0;
    if (len == 0) return -1;
    *out = malloc(len + 1);
    memcpy(*out, ver, len + 1);
    return 0;
}

gg_ggdir_t *loader_find_dirs(size_t *out_count, const char *pattern) {
    const char *base = "/data/user/0";
    DIR *dp = opendir(base);
    if (!dp) { *out_count = 0; return NULL; }

    size_t cap = 8, n = 0;
    gg_ggdir_t *arr = malloc(cap * sizeof(*arr));
    if (!arr) { closedir(dp); *out_count = 0; return NULL; }

    /* 填充候选目录（包名 pkg, 目录 dir, 是否含核心库） */
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char files[GG_DIR_MAX];
        snprintf(files, sizeof(files), "%s/%s/files", base, de->d_name);

        /* 收集 files/ 下所有子目录 + files/ 自身 */
        /* 容得下 "<files>/<dirent.d_name>"（512 + 255 + 2），否则 gcc 会报
         * -Wformat-truncation；截断的路径指向别处，是静默错误来源。 */
        char cands[16][GG_DIR_MAX + 260];
        int nc = 0;
        DIR *fdp = opendir(files);
        if (fdp) {
            struct dirent *fde;
            while ((fde = readdir(fdp)) != NULL && nc < 16) {
                if (fde->d_name[0] == '.') continue;
                if (nc < 16) {
                    int n = snprintf(cands[nc], sizeof(cands[0]), "%s/%s", files, fde->d_name);
                    /* 万一还是截断（files 已接近上限），宁可不收也不要拿半截路径去 opendir */
                    if (n > 0 && (size_t)n < sizeof(cands[0])) nc++;
                }
            }
            closedir(fdp);
            /* files/ 自身也作为候选（核心可能直接在 files/ 下） */
            if (nc < 16) snprintf(cands[nc++], sizeof(cands[0]), "%s", files);
        }

        for (int c = 0; c < nc; c++) {
            const char *dir = cands[c];
            /* GG- 前缀目录: 完整识别（已知名 + ELF 兜底）;
             * 其他目录: 仅已知名快速检查（避免对海量文件做 I/O 风暴） */
            const char *base_name = strrchr(dir, '/');
            base_name = base_name ? base_name + 1 : dir;
            int gg_named = strncmp(base_name, "GG-", 3) == 0;
            const char *core = gg_named ? find_lib5(dir) : find_lib5_known(dir);
            /* 只保留含核心库的目录（避免对每个普通包跑 dumpsys 造成卡顿） */
            if (!core) continue;
            if (n == cap) {
                cap *= 2;
                gg_ggdir_t *na = realloc(arr, cap * sizeof(*na));
                if (!na) break;
                arr = na;
            }
            gg_ggdir_t *g = &arr[n++];
            memset(g, 0, sizeof(*g));
            /* g->dir 定长 512。超长路径宁可丢弃也不能截断：
             * 半截路径会让后续 opendir/替换装到错误位置。
             * 下面已用返回���判断，截断时 n-- 丢弃；gcc 的 -Wformat-truncation
             * 只看得到「源可能更长」，看不到这个检查，故在此局部抑制。 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
            {
                int dn = snprintf(g->dir, sizeof(g->dir), "%s", dir);
                if (dn < 0 || (size_t)dn >= sizeof(g->dir)) { n--; continue; }
            }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
            snprintf(g->pkg, sizeof(g->pkg), "%s", de->d_name);
            g->has_lib5 = core != NULL;
            {
                char *v = NULL;
                /* 版本来源优先级: version.gg（权威）→ dumpsys package */
                if (version_from_gg_file(g->dir, &v) != 0 || !v) {
                    if (version_from_package(g->pkg, &v) != 0 || !v) {
                        snprintf(g->version, sizeof(g->version), "unknown");
                        v = NULL;
                    }
                }
                if (v) {
                    snprintf(g->version, sizeof(g->version), "%s", v);
                    free(v);
                }
            }
            if (pattern && *pattern) {
                int m = version_match_family(g->version, pattern);
                g->family_match = m > 0 ? 1 : 0;
            } else {
                g->family_match = 1;
            }
            g->stub_installed = 0;
            /* 判断桩是否已安装: 核心被替换且存在 .orig 备份 */
            if (g->has_lib5) {
                char p[GG_DIR_MAX + 32];
                const char *l5 = find_lib5(g->dir);
                if (l5) {
                    snprintf(p, sizeof(p), "%s/%s.orig", g->dir, l5);
                    g->stub_installed = file_exists(p);
                }
            }
            g->hook_loaded = loader_verify_hook(g);
        }
    }
    closedir(dp);
    *out_count = n;
    return arr;
}

int loader_load(gg_ggdir_t *g, const char *pattern, int strict) {
    const char *l5 = find_lib5(g->dir);
    if (!l5) return -GGERR_NOT_FOUND;

    /* 版本族校验 */
    if (pattern && *pattern && version_syntactic(g->version)) {
        int m = version_match_family(g->version, pattern);
        if (m == 0 && strict) return -GGERR_VERSION_MISMATCH;
        if (m == 0 && !strict) {
            /* 非严格模式: 告警继续（日志由上层记录） */
        }
    }

    char stub_src[GG_DIR_MAX + 64];
    char hook_src[GG_DIR_MAX + 64];
    snprintf(stub_src, sizeof(stub_src), "%s/lib5_stub", g_assets_dir);
    snprintf(hook_src, sizeof(hook_src), "%s/libKernelGg.so", g_assets_dir);
    if (!file_exists(stub_src) || !file_exists(hook_src)) return -GGERR_INTERNAL;

    char orig_path[GG_DIR_MAX + 32];
    char stub_dst[GG_DIR_MAX + 32];
    char hook_dst[GG_DIR_MAX + 32];
    snprintf(orig_path, sizeof(orig_path), "%s/%s.orig", g->dir, l5);
    snprintf(stub_dst, sizeof(stub_dst), "%s/%s", g->dir, l5);
    snprintf(hook_dst, sizeof(hook_dst), "%s/libKernelGg.so", g->dir);

    /* 1) 备份原库（已备份则跳过） */
    if (!file_exists(orig_path)) {
        if (copy_file(stub_dst, orig_path, 0755) < 0) return -GGERR_PERM;
    }
    /* 2) 安装桩（rename 覆盖, 见 install_file 注释: 直接 O_TRUNC 会 ETXTBSY） */
    if (install_file(stub_src, stub_dst, 0777) < 0) return -GGERR_PERM;
    /* 3) 安装 hook（同样走 rename, 原子替换） */
    if (install_file(hook_src, hook_dst, 0777) < 0) return -GGERR_PERM;
    /* 4) 写驱动协议环境配置（桩启动时 setenv 给 hook） */
    {
        char conf[GG_DIR_MAX + 32];
        snprintf(conf, sizeof(conf), "%s/lib5_hook.conf", g->dir);
        FILE *cf = fopen(conf, "w");
        if (cf) {
            fprintf(cf, "AQ_KO_PROTO=%s\n", g_hook_proto);
            if (g_hook_dev[0]) fprintf(cf, "AQ_KO_DEV=%s\n", g_hook_dev);
            if (g_hook_key[0]) fprintf(cf, "AQ_KO_KEY=%s\n", g_hook_key);
            fclose(cf);
            chmod(conf, 0644);
        }
    }

    g->stub_installed = 1;
    g->hook_loaded = loader_verify_hook(g);
    return 0;
}

int loader_verify_hook(const gg_ggdir_t *g) {
    /* 扫描所有进程 maps，找加载了 <g->dir>/libKernelGg.so 的进程 */
    DIR *dp = opendir("/proc");
    if (!dp) return 0;
    struct dirent *de;
    int found = 0;
    while ((de = readdir(dp)) != NULL && !found) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char path[320];   /* "/proc/<pid>/maps"：dirent 的 d_name 最长 255，64 会截断 */
        snprintf(path, sizeof(path), "/proc/%s/maps", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "libKernelGg.so")) {
                found = 1;
                break;
            }
        }
        fclose(f);
    }
    closedir(dp);
    (void)g;
    return found;
}

int loader_restore(gg_ggdir_t *g) {
    const char *l5 = find_lib5(g->dir);
    if (!l5) return -GGERR_NOT_FOUND;
    char orig_path[GG_DIR_MAX + 32];
    char stub_dst[GG_DIR_MAX + 32];
    snprintf(orig_path, sizeof(orig_path), "%s/%s.orig", g->dir, l5);
    snprintf(stub_dst, sizeof(stub_dst), "%s/%s", g->dir, l5);
    if (file_exists(orig_path)) {
        if (install_file(orig_path, stub_dst, 0755) < 0) return -GGERR_PERM;
        remove(orig_path);
    }
    char hook_dst[GG_DIR_MAX + 32];
    snprintf(hook_dst, sizeof(hook_dst), "%s/libKernelGg.so", g->dir);
    remove(hook_dst);
    g->stub_installed = 0;
    g->hook_loaded = 0;
    return 0;
}
