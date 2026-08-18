/* elfsym.h — ELF64 动态符号表查询（纯静态解析）
 *
 * gg_resolve_symbol 的本地静态层：在模块 ELF 文件的 .dynsym/.symtab 中
 * 找"不大于给定偏移的最近导出符号"，让 AI 在无 IDA 时也能拿到函数名。
 */
#ifndef GG_AGENTD_ELFSYM_H
#define GG_AGENTD_ELFSYM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 符号种类（ELF STT_*） */
#define ELFSYM_NOTYPE  0
#define ELFSYM_OBJECT  1
#define ELFSYM_FUNC    2

/* 在 ELF64 文件的 .dynsym（优先）/.symtab 中找"不大于 offset 的最近符号"。
 *
 * path:   模块文件路径（设备端可读；base.apk 内 so 无独立文件则跳过）
 * offset: 模块加载基址起的虚拟偏移（= 运行时地址 - 模块加载基址）
 * name:   命中时输出符号名（NUL 结尾）
 * value:  命中时输出 st_value（模块内虚拟偏移）
 * delta:  命中时输出 offset - st_value（0 = 精确命中）
 * kind:   命中时输出 ELFSYM_*（FUNC/OBJECT/NOTYPE）
 *
 * 返回: 1=命中 0=未命中（无符号表或范围内无符号） -1=文件不可读/非 64 位 ELF
 */
int elfsym_nearest(const char *path, uint64_t offset,
                   char *name, size_t name_sz,
                   uint64_t *value, uint64_t *delta, int *kind);

#ifdef __cplusplus
}
#endif

#endif /* GG_AGENTD_ELFSYM_H */
