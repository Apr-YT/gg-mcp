/* version.h — GG 版本提取与版本族匹配（核心需求: 6.12.xx 同族可加载） */
#ifndef GG_AGENTD_VERSION_H
#define GG_AGENTD_VERSION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gg_ver {
    int major, minor, patch;
    int has_patch;      /* "6.12" 无补丁号时 has_patch=0 */
    int valid;
} gg_ver_t;

/* 解析 "6.12.9" / "6.12" / "v6.12.9-beta" 等 */
int  version_parse(const char *s, gg_ver_t *out);

/* 版本族匹配：
 *   pattern "6.12.*" 或 "6.12" → major/minor 相同即匹配（补丁任意）→ 1
 *   pattern "6.12.9"          → 全等才匹配
 *   返回 1=匹配 0=不匹配 -1=解析失败 */
int  version_match_family(const char *version, const char *pattern);

/* 从 KO 文件名提取版本: "driverQX114_6.12.9.ko" → "6.12.9"
 * 取第一个"含点的数字段"（跳过名字里的数字如 QX114）。
 * 成功返回 0 并填充 out；无版本段返回 -1。 */
int  version_from_ko_name(const char *name, char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

#endif /* GG_AGENTD_VERSION_H */
