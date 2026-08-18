/* loader.h — GG 核心对接编排（外挂圈子加载内核方式）
 *
 * 链路: GG主程序 → su → lib05.so(守护) → exec <ggdir>/lib5.so
 * 方案: 把 <ggdir>/lib5.so 替换为 ELF 桩(lib5_stub, 静态PIE)，
 *       原库备份为 lib5.so.orig；桩设 LD_PRELOAD=libKernelGg.so 后 exec 原库；
 *       hook 拦截 syscall 270(process_vm_readv) → 内核驱动。
 * 版本族: 加载前用 version_match_family 校验（6.12.xx 同族可加载）。
 */
#ifndef GG_AGENTD_LOADER_H
#define GG_AGENTD_LOADER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GG_DIR_MAX   512
#define GG_PKG_MAX   256

typedef struct gg_ggdir {
    char dir[GG_DIR_MAX];       /* /data/user/0/<pkg>/files/GG-xxxx/ */
    char pkg[GG_PKG_MAX];
    char version[64];           /* 从 dumpsys 获取 */
    int  has_lib5;              /* 存在 lib5.so / lib5.so.primary */
    int  family_match;          /* 与目标 pattern 的匹配结果 */
    int  stub_installed;        /* 已安装桩 */
    int  hook_loaded;           /* 检测到 libKernelGg.so 已注入运行中的 GG 进程 */
} gg_ggdir_t;

/* 扫描所有 GG 私有目录（/data/user/0 下各包的 files 目录中的 GG- 前缀目录）。
 * 返回 malloc 数组 + 计数。 */
gg_ggdir_t *loader_find_dirs(size_t *out_count, const char *pattern);

/* 设置 hook 环境（写 lib5_hook.conf 用）:
 * proto: "hook" | "dev" | "qx"；dev/key 可为 NULL */
void loader_set_hook_env(const char *proto, const char *dev, const char *key);

/* 加载指定 GG 目录的核心：备份原库 → 装桩 → 装 hook。
 * pattern: 版本族模式（如 "101.*"）；strict: 族外拒绝。
 * 返回 0 成功；负值对应 wire_protocol 错误码（GGERR_*）。 */
int loader_load(gg_ggdir_t *g, const char *pattern, int strict);

/* 校验运行中的 GG 进程是否已注入 hook（扫描 /proc 下各进程 maps 找 libKernelGg.so）。 */
int loader_verify_hook(const gg_ggdir_t *g);

/* 卸载：恢复原库。 */
int loader_restore(gg_ggdir_t *g);

#ifdef __cplusplus
}
#endif

#endif /* GG_AGENTD_LOADER_H */
