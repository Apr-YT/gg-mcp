/* disasm.c — ARM64 反汇编实现（Capstone 5.x, 静态链接）
 *
 * 编译需: -I<capstone>/include + libcapstone.a（NDK arm64 交叉编译）。
 */
#include "disasm.h"

#include <capstone/capstone.h>

#include <stdio.h>
#include <string.h>

static void hex_bytes(char out[32], const uint8_t *b, int n) {
    int o = 0;
    for (int k = 0; k < n && o < 30; k++) {
        o += snprintf(out + o, (size_t)(32 - o), "%02x ", b[k]);
    }
    if (o > 0 && out[o - 1] == ' ') out[o - 1] = 0;
}

int disasm_a64(const uint8_t *code, size_t size, uint64_t addr,
               char mnem[][64], char ops[][128], char bytes[][32],
               char comment[][64], int cap) {
    csh handle;
    if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle) != CS_ERR_OK) return -1;
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);   /* 需要 groups 生成注释 */

    cs_insn *insn = NULL;
    size_t n = cs_disasm(handle, code, size, addr, 0, &insn);
    if (n == 0) {
        cs_close(&handle);
        return 0;
    }
    int count = (int)(n < (size_t)cap ? n : (size_t)cap);
    for (int i = 0; i < count; i++) {
        snprintf(mnem[i], 64, "%s", insn[i].mnemonic);
        snprintf(ops[i], 128, "%s", insn[i].op_str);
        hex_bytes(bytes[i], insn[i].bytes, (int)insn[i].size);
        /* 注释：按 Capstone 分组 + 指令 ID 启发式 */
        comment[i][0] = 0;
        if (insn[i].detail) {
            int is_call = 0, is_jmp = 0, is_ret = 0;
            for (unsigned g = 0; g < insn[i].detail->groups_count; g++) {
                uint8_t grp = insn[i].detail->groups[g];
                if (grp == CS_GRP_CALL) is_call = 1;
                else if (grp == CS_GRP_JUMP || grp == CS_GRP_BRANCH_RELATIVE) is_jmp = 1;
                else if (grp == CS_GRP_RET) is_ret = 1;
            }
            if (is_call) {
                /* 分支目标: 解析 op_str 里的 #0x... 或 0x... */
                const char *o = insn[i].op_str;
                const char *hash = strchr(o, '#');
                const char *tgt = hash ? hash + 1 : o;
                if (tgt[0] == '0' && (tgt[1] == 'x' || tgt[1] == 'X'))
                    snprintf(comment[i], 64, "call %s", tgt);
                else
                    snprintf(comment[i], 64, "call");
            } else if (is_jmp) {
                const char *o = insn[i].op_str;
                const char *hash = strchr(o, '#');
                const char *tgt = hash ? hash + 1 : o;
                if (tgt[0] == '0' && (tgt[1] == 'x' || tgt[1] == 'X'))
                    snprintf(comment[i], 64, "jump %s", tgt);
                else
                    snprintf(comment[i], 64, "jump");
            } else if (is_ret) {
                snprintf(comment[i], 64, "return");
            }
        }
        if (comment[i][0] == 0) {
            const char *mn = mnem[i];
            if (strncmp(mn, "ldr", 3) == 0 || strncmp(mn, "ldp", 3) == 0 ||
                strncmp(mn, "ldrsw", 5) == 0)
                snprintf(comment[i], 64, "load");
            else if (strncmp(mn, "str", 3) == 0 || strncmp(mn, "stp", 3) == 0)
                snprintf(comment[i], 64, "store");
            else if (strncmp(mn, "adrp", 4) == 0 || strncmp(mn, "adr", 3) == 0)
                snprintf(comment[i], 64, "addr");
            else if (strncmp(mn, "svc", 3) == 0)
                snprintf(comment[i], 64, "syscall");
        }
    }
    cs_free(insn, n);
    cs_close(&handle);
    return count;
}
