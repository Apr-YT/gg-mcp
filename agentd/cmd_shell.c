/* cmd_shell.c — gg_shell 工具实现（自 main.c 拆出，2026-08-28）
 *
 * 安全模型（与 tests/unit/test_shell_security.c 共享 shell_util.h 单一事实源）：
 *   - 不经 sh -c，fork + execvp 直执行 argv（走 PATH）——天然免疫 ; | & $() 注入
 *   - argv 按空白切分（shell_split_argv，无引号/变量展开）
 *   - 命令名 basename 白名单（shell_argv_allowed：ps/ls/cat/dumpsys/getprop/id）
 *   - 双闸门：shell_enabled 配置开关 + safe_mode 只读拒绝
 * 子进程只调用 async-signal-safe 函数（dup2/close/execvp），多线程 fork 安全。
 */
#include "engine_internal.h"
#include "shell_util.h"

#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* fork+execvp 直执行 argv, capture stdout 到 out(outsz)。
 * 返回 0 成功, -1 失败; 成功时 *rc = 退出码。 */
static int shell_execvp(char **argv, char *out, size_t outsz, int *rc) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        if (fds[1] != STDOUT_FILENO) close(fds[1]);
        close(fds[0]);
        execvp(argv[0], argv);
        _exit(127);   /* exec 失败 */
    }
    close(fds[1]);
    size_t got = 0;
    ssize_t n;
    while (got < outsz - 1 && (n = read(fds[0], out + got, outsz - 1 - got)) > 0) {
        got += (size_t)n;
    }
    out[got] = 0;
    close(fds[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) == pid && WIFEXITED(status)) *rc = WEXITSTATUS(status);
    else *rc = -1;
    return 0;
}

void h_shell(int fd, uint32_t seq, cJSON *p) {
    if (!g_eng.shell_enabled) { send_err(fd, GGCMD_SHELL, seq, GGERR_SHELL_DISABLED, "shell disabled by config"); return; }
    if (g_eng.safe_mode) { send_err(fd, GGCMD_SHELL, seq, GGERR_SAFE_MODE, "safe mode: shell denied"); return; }
    const char *cmd = cJSON_GetObjectItem(p, "cmd") ? cJSON_GetObjectItem(p, "cmd")->valuestring : NULL;
    if (!cmd || !*cmd) { send_err(fd, GGCMD_SHELL, seq, GGERR_BAD_REQUEST, "cmd required"); return; }

    char buf[4096];
    if (strlen(cmd) >= sizeof(buf)) { send_err(fd, GGCMD_SHELL, seq, GGERR_BAD_REQUEST, "cmd too long"); return; }
    strncpy(buf, cmd, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    char *argv[64];
    int argc = shell_split_argv(buf, argv, 64);
    if (argc <= 0) { send_err(fd, GGCMD_SHELL, seq, GGERR_BAD_REQUEST, "empty command"); return; }
    if (!shell_argv_allowed(argv[0])) { send_err(fd, GGCMD_SHELL, seq, GGERR_BAD_REQUEST, "command not in whitelist"); return; }

    char out[8192];
    int rc = 0;
    if (shell_execvp(argv, out, sizeof(out), &rc) != 0) {
        send_err(fd, GGCMD_SHELL, seq, GGERR_INTERNAL, "exec failed");
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "output", out);
    cJSON_AddNumberToObject(r, "rc", rc);
    send_ok(fd, GGCMD_SHELL, seq, r);
    cJSON_Delete(r);
}