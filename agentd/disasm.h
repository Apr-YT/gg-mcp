/* disasm.h — ARM64 反汇编（Capstone 5.x）
 *
 * 供 gg_disassemble 工具使用：读取目标进程内存后用 Capstone 反汇编，
 * 返回结构化指令（地址/字节/助记符/操作数/注释）。
 */
#ifndef GG_AGENTD_DISASM_H
#define GG_AGENTD_DISASM_H

#include <stddef.h>
#include <stdint.h>

#define GG_DISASM_MAX_INSNS 256

/* 反汇编 code[0..size)（起始虚拟地址 addr）。
 * 结果写入 mnem[i]/ops[i]/bytes[i]（cap 条上限, 建议 GG_DISASM_MAX_INSNS）。
 * 返回实际指令数；-1 引擎初始化失败。 */
int disasm_a64(const uint8_t *code, size_t size, uint64_t addr,
               char mnem[][64], char ops[][128], char bytes[][32],
               char comment[][64], int cap);

#endif /* GG_AGENTD_DISASM_H */
