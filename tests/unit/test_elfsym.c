/* test_elfsym.c — elfsym_nearest 单测（自包含 + 构造 ELF64 全面验证 + 可选真实 ELF）
 * 用法: test_elfsym.exe [elf路径...]   提供路径则对该文件做交叉验证
 */
#include "../../agentd/elfsym.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
static void check(int cond, const char *m) {
    printf("%s %s\n", cond ? "ok  :" : "FAIL:", m);
    if (!cond) fails++;
}

/* ---------- 构造最小 ELF64（LSB/MSB；可只含 .dynsym 或 .symtab） ---------- */
static void put16(uint8_t *b, int be, size_t off, uint16_t v) {
    if (be) { b[off] = (uint8_t)(v >> 8); b[off + 1] = (uint8_t)v; }
    else    { b[off] = (uint8_t)v; b[off + 1] = (uint8_t)(v >> 8); }
}
static void put32(uint8_t *b, int be, size_t off, uint32_t v) {
    if (be) { b[off] = (uint8_t)(v >> 24); b[off+1] = (uint8_t)(v >> 16); b[off+2] = (uint8_t)(v >> 8); b[off+3] = (uint8_t)v; }
    else    { b[off] = (uint8_t)v; b[off+1] = (uint8_t)(v >> 8); b[off+2] = (uint8_t)(v >> 16); b[off+3] = (uint8_t)(v >> 24); }
}
static void put64(uint8_t *b, int be, size_t off, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        int shift = be ? (56 - 8 * i) : (8 * i);
        b[off + i] = (uint8_t)(v >> shift);
    }
}

/* 符号: {name_off, st_info, st_shndx, st_value} */
typedef struct test_sym {
    uint32_t name_off;
    uint8_t  info;
    uint16_t shndx;
    uint64_t value;
} test_sym_t;

/* 生成最小 ELF64: 头 + 节头表(3节) + .dynsym(或.symtab) + .dynstr。
 * strsz 为字符串表字节数（内含 \0，不能用 strlen——首符号后即有 \0）。
 * 返回 0 成功（文件已写）；-1 失败。 */
static int make_elf(const char *path, int be, int use_symtab,
                    const test_sym_t *syms, size_t nsyms,
                    const char *strtab, size_t strsz) {
    uint8_t *f = calloc(1, 4096);
    if (!f) return -1;
    size_t shoff = 64;                       /* 节头表紧随 ELF 头 */
    size_t sym_off = shoff + 3 * 64;         /* 3 节头 */
    size_t str_off = sym_off + nsyms * 24;

    /* ELF 头 */
    f[0] = 0x7f; f[1] = 'E'; f[2] = 'L'; f[3] = 'F';
    f[4] = 2;                                /* ELFCLASS64 */
    f[5] = (uint8_t)(be ? 2 : 1);            /* EI_DATA */
    put64(f, be, 40, shoff);                 /* e_shoff */
    put16(f, be, 58, 64);                    /* e_shentsize */
    put16(f, be, 60, 3);                     /* e_shnum */

    /* 节头表 */
    uint8_t *sh;
    sh = f + shoff + 0 * 64;                 /* 节0 NULL */
    sh = f + shoff + 1 * 64;                 /* 节1 符号表 */
    put32(sh, be, 4, (uint32_t)(use_symtab ? 2 : 11));  /* SHT_SYMTAB/DYNSYM */
    put64(sh, be, 24, sym_off);
    put64(sh, be, 32, nsyms * 24);
    put32(sh, be, 40, 2);                    /* sh_link -> 节2 */
    sh = f + shoff + 2 * 64;                 /* 节2 字符串表 */
    put32(sh, be, 4, 3);                     /* SHT_STRTAB */
    put64(sh, be, 24, str_off);
    put64(sh, be, 32, strsz);

    /* 符号表 */
    for (size_t i = 0; i < nsyms; i++) {
        uint8_t *e = f + sym_off + i * 24;
        put32(e, be, 0, syms[i].name_off);
        e[4] = syms[i].info;
        put16(e, be, 6, syms[i].shndx);
        put64(e, be, 8, syms[i].value);
    }
    memcpy(f + str_off, strtab, strsz);

    FILE *fp = fopen(path, "wb");
    if (!fp) { free(f); return -1; }
    fwrite(f, 1, str_off + strsz, fp);
    fclose(fp);
    free(f);
    return 0;
}

int main(int argc, char **argv) {
    /* 1) 参数/错误路径 */
    check(elfsym_nearest(NULL, 0, NULL, 0, NULL, NULL, NULL) == -1, "null args -> -1");
    char name[256];
    uint64_t val = 0, delta = 0;
    int kind = 0;
    check(elfsym_nearest("/nonexistent/file.so", 0x100, name, sizeof(name), &val, &delta, &kind) == -1,
          "missing file -> -1");

    /* 2) 构造 ELF 全面验证 */
    {
        /* 符号: func_a@0x1000 func_b@0x2000 obj_c@0x3000(OBJECT) import_d@0(UNDEF 跳过) */
        test_sym_t syms[5] = {
            {0, 0x00, 0, 0},                      /* 符号0: 空名 */
            {0, 0x12, 1, 0x1000},                 /* func_a FUNC (name_off 下面修正) */
            {0, 0x12, 1, 0x2000},                 /* func_b FUNC */
            {0, 0x11, 1, 0x3000},                 /* obj_c OBJECT */
            {0, 0x12, 0, 0x4000},                 /* import_d UNDEF(跳过) */
        };
        /* 注意 strtab 不以 \0 开头（便于索引），name_off 从 0 起；
         * sizeof(strtab) 含内嵌 \0（strlen 会在首个 \0 停, 不可用） */
        static const char strtab[] = "func_a\0func_b\0obj_c\0import_d\0";
        syms[1].name_off = 0; syms[2].name_off = 7; syms[3].name_off = 14;
        syms[4].name_off = 20;

        for (int be = 0; be < 2; be++) {
            const char *tag = be ? "MSB" : "LSB";
            const char *path = be ? "elfsym_msb.bin" : "elfsym_lsb.bin";
            check(make_elf(path, be, 0, syms, 5, strtab, sizeof(strtab)) == 0, "make_elf LSB/MSB");
            char m[128];

            int rc = elfsym_nearest(path, 0x1000, name, sizeof(name), &val, &delta, &kind);
            snprintf(m, sizeof(m), "%s: offset=0x1000 -> func_a delta=0 kind=FUNC", tag);
            check(rc == 1 && strcmp(name, "func_a") == 0 && val == 0x1000 && delta == 0 && kind == ELFSYM_FUNC, m);

            rc = elfsym_nearest(path, 0x1500, name, sizeof(name), &val, &delta, &kind);
            snprintf(m, sizeof(m), "%s: offset=0x1500 -> func_a delta=0x500", tag);
            check(rc == 1 && strcmp(name, "func_a") == 0 && delta == 0x500, m);

            rc = elfsym_nearest(path, 0x2500, name, sizeof(name), &val, &delta, &kind);
            snprintf(m, sizeof(m), "%s: offset=0x2500 -> func_b delta=0x500", tag);
            check(rc == 1 && strcmp(name, "func_b") == 0 && delta == 0x500, m);

            rc = elfsym_nearest(path, 0x900, name, sizeof(name), &val, &delta, &kind);
            snprintf(m, sizeof(m), "%s: offset=0x900 无最近符号 -> 0", tag);
            check(rc == 0, m);

            rc = elfsym_nearest(path, 0x3000, name, sizeof(name), &val, &delta, &kind);
            snprintf(m, sizeof(m), "%s: offset=0x3000 -> obj_c kind=OBJECT", tag);
            check(rc == 1 && strcmp(name, "obj_c") == 0 && kind == ELFSYM_OBJECT, m);

            rc = elfsym_nearest(path, 0x4000, name, sizeof(name), &val, &delta, &kind);
            snprintf(m, sizeof(m), "%s: offset=0x4000 -> obj_c(最近) 非 import_d", tag);
            check(rc == 1 && strcmp(name, "obj_c") == 0, m);

            rc = elfsym_nearest(path, 0x5000, name, sizeof(name), &val, &delta, &kind);
            snprintf(m, sizeof(m), "%s: offset=0x5000 -> obj_c delta=0x2000", tag);
            check(rc == 1 && strcmp(name, "obj_c") == 0 && delta == 0x2000, m);

            remove(path);
        }

        /* .symtab 回退: 只有 .symtab 时也能解析 */
        check(make_elf("elfsym_st.bin", 0, 1, syms, 5, strtab, sizeof(strtab)) == 0, "make_elf symtab-only");
        int rc = elfsym_nearest("elfsym_st.bin", 0x1000, name, sizeof(name), &val, &delta, &kind);
        check(rc == 1 && strcmp(name, "func_a") == 0 && delta == 0, "symtab 回退: func_a delta=0");
        remove("elfsym_st.bin");

        /* 畸形字符串表：整段没有 NUL 结尾，且有一个符号的 st_name 落在表外。
         * 旧实现只校验「起点在表内」，随后 strlen 会一路读穿堆（agentd 是 root），
         * 读到的堆内容还会随 JSON 返回。修复后：名字按可用长度截断，越界项跳过。 */
        {
            static const char bad_strtab[] = "truncated";   /* 9 字节有效，无 NUL */
            test_sym_t bad_syms[2] = {
                {0, 0x12, 1, 0x1000},   /* 名字从表头开始 */
                {0, 0x12, 1, 0x2000},   /* name_off 落到表尾之外 */
            };
            bad_syms[1].name_off = 9;
            check(make_elf("elfsym_badstr.bin", 0, 0, bad_syms, 2, bad_strtab, 9) == 0,
                  "make_elf: 无 NUL 字符串表");
            int r = elfsym_nearest("elfsym_badstr.bin", 0x1000, name, sizeof(name), &val, &delta, &kind);
            check(r == 1 && strcmp(name, "truncated") == 0,
                  "无 NUL 表: 名字按可用长度截断，不越界");
            r = elfsym_nearest("elfsym_badstr.bin", 0x2000, name, sizeof(name), &val, &delta, &kind);
            check(r == 1 && strcmp(name, "truncated") == 0,
                  "st_name 越界: 该项被跳过，回落到最近合法符号");
            remove("elfsym_badstr.bin");
        }

        /* 损坏/非 ELF 文件 -> -1 */
        FILE *bad = fopen("elfsym_bad.bin", "wb");
        fputs("not an elf file at all", bad);
        fclose(bad);
        check(elfsym_nearest("elfsym_bad.bin", 0x10, name, sizeof(name), &val, &delta, &kind) == -1,
              "非 ELF 文件 -> -1");
        remove("elfsym_bad.bin");

        /* 32 位 ELF -> -1 */
        {
            uint8_t elf32[64];
            memset(elf32, 0, sizeof(elf32));
            elf32[0] = 0x7f; elf32[1] = 'E'; elf32[2] = 'L'; elf32[3] = 'F';
            elf32[4] = 1;   /* ELFCLASS32 */
            FILE *f = fopen("elfsym_32.bin", "wb");
            fwrite(elf32, 1, sizeof(elf32), f);
            fclose(f);
            check(elfsym_nearest("elfsym_32.bin", 0x10, name, sizeof(name), &val, &delta, &kind) == -1,
                  "32 位 ELF -> -1");
            remove("elfsym_32.bin");
        }
    }

    /* 3) 真实 ELF 交叉验证（argv 提供路径时）
       ⚠️ 不要断言符号的绝对地址。被测二进制每次重编布局都会变（曾有符号在 0x1908，
       重新构建后挪到 0x860），断言绝对值会让测试随构建产物随机变红。
       这里只校验 elfsym_nearest 的核心不变量：delta == 查询偏移 - 符号值。 */
    for (int i = 1; i < argc; i++) {
        const char *path = argv[i];
        printf("== %s ==\n", path);
        int rc = elfsym_nearest(path, 0x1908, name, sizeof(name), &val, &delta, &kind);
        check(rc >= -1 && rc <= 1, "rc in {-1,0,1} (无崩溃)");
        if (!(rc == 1 && kind == ELFSYM_FUNC)) {
            printf("    (0x1908 处无 FUNC 符号: rc=%d name=%s — 跳过针对该文件的断言)\n",
                   rc, name[0] ? name : "(none)");
            continue;
        }
        uint64_t base = val;
        char base_name[128];
        snprintf(base_name, sizeof(base_name), "%s", name);
        check(delta == 0x1908 - base, "delta == 查询偏移 - 符号值");
        printf("    got: name=%s val=0x%llx delta=0x%llx kind=%d\n",
               name, (unsigned long long)val, (unsigned long long)delta, kind);
        /* 符号内部偏移：仍命中同一符号，delta 等于相对位移 */
        rc = elfsym_nearest(path, base + 0x100, name, sizeof(name), &val, &delta, &kind);
        check(rc == 1 && strcmp(name, base_name) == 0 && delta == 0x100,
              "符号内部 +0x100 -> 同一符号 delta=0x100");
        /* 符号之前（无定义符号覆盖） */
        rc = elfsym_nearest(path, 0x100, name, sizeof(name), &val, &delta, &kind);
        printf("    offset=0x100 -> rc=%d (0 = 无最近符号, 符合预期)\n", rc);
    }

    printf(fails ? "\n== FAILED: %d ==\n" : "\n== ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
