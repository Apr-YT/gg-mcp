#ifndef GG_SHELL_UTIL_H
#define GG_SHELL_UTIL_H

/* 安全 shell 纯逻辑 helper：拆分 argv + 白名单校验 basename。
 * 不含 POSIX 执行 (fork/execvp)，该部分在 main.c 的 shell_execvp。
 * 被 main.c 与单元测试共享，作为单一事实源 —— 改这里即同时改测试守护。 */
#include <string.h>

/* 按空白原地拆分 cmd 为 argv（原地修改 cmd 字符串），返回 argc（含结尾 NULL 预留）。
 * 不展开引号/变量，仅按空白切分 —— 注入面只剩"命令名"，由白名单 basename 兜底。 */
static inline int shell_split_argv(char *cmd, char **argv, int max_argv) {
    int argc = 0;
    char *p = cmd;
    while (*p && argc < max_argv - 1) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if (*p) { *p = 0; p++; }
    }
    argv[argc] = NULL;
    return argc;
}

/* 仅校验 argv[0] 的 basename 是否在白名单（不进 shell，天然免疫 ; & | $() 等注入） */
static inline int shell_argv_allowed(const char *prog) {
    if (!prog || !*prog) return 0;
    const char *base = strrchr(prog, '/');
    base = base ? base + 1 : prog;
    static const char *allowed[] = { "ps", "ls", "cat", "dumpsys", "getprop", "id" };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strcmp(base, allowed[i]) == 0) return 1;
    }
    return 0;
}

#endif /* GG_SHELL_UTIL_H */
