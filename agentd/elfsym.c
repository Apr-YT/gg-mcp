/* elfsym.c — ELF64 符号表解析（.dynsym 优先，退 .symtab）
 *
 * 只做"读文件 + 查最近导出符号"，无外部依赖（stdio）。
 * 支持小端/大端；只支持 64 位 ELF（本项目 arm64-only）。
 */
#include "elfsym.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELF_MAG0 0x7f
#define ELF_MAG1 'E'
#define ELF_MAG2 'L'
#define ELF_MAG3 'F'

#define EI_CLASS   4
#define EI_DATA    5
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

#define SHT_SYMTAB  2
#define SHT_DYNSYM  11

#define SHN_UNDEF   0

/* ELF64 头关键偏移（本文件按偏移读，避免依赖系统 elf.h） */
#define EH_E_SH_OFF   40   /* e_shoff  u64 */
#define EH_E_SHENTSZ  58   /* e_shentsize u16 */
#define EH_E_SHNUM    60   /* e_shnum  u16 */

/* ELF64 节头关键偏移 */
#define SH_SH_TYPE   4    /* sh_type  u32 */
#define SH_SH_OFFSET 24   /* sh_offset u64 */
#define SH_SH_SIZE   32   /* sh_size  u64 */
#define SH_SH_LINK   40   /* sh_link  u32 */

/* ELF64 符号项关键偏移（sizeof 24） */
#define SYM_ST_NAME  0    /* u32 */
#define SYM_ST_INFO  4    /* u8 */
#define SYM_ST_VALUE 8    /* u64 */
#define SYM_ST_SIZE  16   /* u64 */
#define SYM_ENT_SIZE 24

typedef struct elf_rd {
    FILE *f;
    int   be;   /* 大端 */
} elf_rd_t;

static uint16_t rd_u16(elf_rd_t *r, const uint8_t *p) {
    if (r->be) return (uint16_t)((p[0] << 8) | p[1]);
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_u32(elf_rd_t *r, const uint8_t *p) {
    if (r->be) return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                      ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(elf_rd_t *r, const uint8_t *p) {
    if (r->be) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
        return v;
    }
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static int read_at(elf_rd_t *r, uint64_t off, void *buf, size_t len) {
    if (fseek(r->f, (long)off, SEEK_SET) != 0) return -1;
    return fread(buf, 1, len, r->f) == len ? 0 : -1;
}

/* 逐符号扫描，返回最近命中数（0/1）；-1 读取失败 */
static int scan_table(elf_rd_t *r, uint64_t sym_off, uint64_t sym_size,
                      uint64_t str_off, uint64_t str_size,
                      uint64_t offset,
                      char *name_out, size_t name_sz,
                      uint64_t *value_out, uint64_t *delta_out, int *kind_out) {
    uint8_t ent[SYM_ENT_SIZE];
    uint8_t *strtab = NULL;
    if (str_size > 0) {
        strtab = malloc((size_t)str_size);
        if (!strtab) return -1;
        if (read_at(r, str_off, strtab, (size_t)str_size) != 0) {
            free(strtab);
            return -1;
        }
    }

    uint64_t n = sym_size / SYM_ENT_SIZE;
    uint64_t best_val = 0;
    const char *best_name = NULL;
    size_t best_name_len = 0;
    uint64_t best_delta = 0;
    int best_kind = ELFSYM_NOTYPE;

    for (uint64_t i = 0; i < n; i++) {
        if (read_at(r, sym_off + i * SYM_ENT_SIZE, ent, sizeof(ent)) != 0) {
            free(strtab);
            return -1;
        }
        uint32_t st_name = rd_u32(r, ent + SYM_ST_NAME);
        uint8_t  st_info = ent[SYM_ST_INFO];
        uint64_t st_value = rd_u64(r, ent + SYM_ST_VALUE);
        uint16_t st_shndx = rd_u16(r, ent + 6);
        int st_kind = st_info & 0x0f;
        const char *nm = (strtab && st_name < str_size) ? (const char *)strtab + st_name : NULL;
        if (!nm || !nm[0] || nm[0] == '$') continue;   /* 空名 / ARM 映射符号($a/$d/$x) 跳过 */

        if (st_value == 0 || st_value > offset) continue;
        if (st_shndx == SHN_UNDEF) continue;           /* 未定义（导入）符号跳过 */
        /* 取"不大于 offset 且 st_value 最大"的符号 = 最近在前符号（含精确覆盖） */
        if (st_value > best_val) {
            best_val = st_value;
            best_kind = st_kind;
            best_delta = offset - st_value;
            best_name = nm;
            best_name_len = strlen(nm);
        }
    }

    int rc = 0;
    if (best_name && best_name_len > 0) {
        size_t cp = best_name_len < name_sz ? best_name_len : name_sz - 1;
        memcpy(name_out, best_name, cp);
        name_out[cp] = 0;
        *value_out = best_val;
        *delta_out = best_delta;
        *kind_out = best_kind;
        rc = 1;
    }
    free(strtab);
    return rc;
}

int elfsym_nearest(const char *path, uint64_t offset,
                   char *name, size_t name_sz,
                   uint64_t *value, uint64_t *delta, int *kind) {
    if (!path || !*path || !name || name_sz == 0) return -1;
    name[0] = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    elf_rd_t r;
    r.f = f;
    r.be = 0;

    uint8_t eh[64];
    if (read_at(&r, 0, eh, sizeof(eh)) != 0 ||
        eh[0] != ELF_MAG0 || eh[1] != ELF_MAG1 || eh[2] != ELF_MAG2 || eh[3] != ELF_MAG3) {
        fclose(f);
        return -1;
    }
    if (eh[EI_CLASS] != ELFCLASS64) {   /* 仅支持 64 位 ELF */
        fclose(f);
        return -1;
    }
    r.be = (eh[EI_DATA] == ELFDATA2MSB);

    uint64_t sh_off = rd_u64(&r, eh + EH_E_SH_OFF);
    uint16_t sh_entsz = rd_u16(&r, eh + EH_E_SHENTSZ);
    uint16_t sh_num = rd_u16(&r, eh + EH_E_SHNUM);
    if (sh_entsz < 64 || sh_num == 0) { fclose(f); return 0; }

    uint8_t sh[64];
    /* 第一遍：收集 DYNSYM 与 SYMTAB；第二遍用 DYNSYM（无则 SYMTAB） */
    uint64_t ds_off = 0, ds_size = 0;
    uint64_t st_off = 0, st_size = 0;
    uint32_t ds_link = 0, st_link = 0;

    for (uint16_t i = 0; i < sh_num; i++) {
        uint64_t sec_off = sh_off + (uint64_t)i * sh_entsz;
        if (read_at(&r, sec_off, sh, 64) != 0) continue;
        uint32_t type = rd_u32(&r, sh + SH_SH_TYPE);
        if (type == SHT_DYNSYM && ds_off == 0) {
            ds_off = rd_u64(&r, sh + SH_SH_OFFSET);
            ds_size = rd_u64(&r, sh + SH_SH_SIZE);
            ds_link = rd_u32(&r, sh + SH_SH_LINK);   /* sh_link 是 Elf64_Word(u32), 用 u16 读会在大端文件上读成 0 */
        } else if (type == SHT_SYMTAB && st_off == 0) {
            st_off = rd_u64(&r, sh + SH_SH_OFFSET);
            st_size = rd_u64(&r, sh + SH_SH_SIZE);
            st_link = rd_u32(&r, sh + SH_SH_LINK);
        }
    }

    /* 取符号表对应字符串表（sh_link 指向的节） */
    uint64_t sym_off = ds_off, sym_size = ds_size, str_off = 0, str_size = 0;
    if (ds_off == 0 && st_off != 0) { sym_off = st_off; sym_size = st_size; }
    if (sym_off == 0) { fclose(f); return 0; }   /* 无符号表 */

    uint32_t str_link = (sym_off == ds_off) ? ds_link : st_link;
    if (str_link != 0 && str_link < sh_num) {
        uint64_t sec_off = sh_off + (uint64_t)str_link * sh_entsz;
        if (read_at(&r, sec_off, sh, 64) == 0) {
            str_off = rd_u64(&r, sh + SH_SH_OFFSET);
            str_size = rd_u64(&r, sh + SH_SH_SIZE);
        }
    }
    if (str_size == 0) { fclose(f); return 0; }

    int rc = scan_table(&r, sym_off, sym_size, str_off, str_size, offset,
                        name, name_sz, value, delta, kind);
    /* .dynsym 命中失败时回退 .symtab（非 stripped 可执行文件常见：dynsym 为空） */
    if (rc != 1 && st_off != 0 && st_off != ds_off) {
        if (st_link != 0 && st_link < sh_num) {
            uint64_t sec_off = sh_off + (uint64_t)st_link * sh_entsz;
            if (read_at(&r, sec_off, sh, 64) == 0) {
                rc = scan_table(&r, st_off, st_size,
                                rd_u64(&r, sh + SH_SH_OFFSET), rd_u64(&r, sh + SH_SH_SIZE),
                                offset, name, name_sz, value, delta, kind);
            }
        }
    }
    fclose(f);
    return rc;
}
