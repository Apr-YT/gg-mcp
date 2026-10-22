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

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

/* 单条 shell 命令的硬上限。白名单里的 `dumpsys` 可能卡在 binder 上、
 * `cat` 可以被指向 FIFO —— 没有这个上限时子进程永不退出。 */
#define SHELL_TIMEOUT_MS 10000
#define SHELL_REAP_TICK_MS 20

static uint64_t shell_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* fork+execvp 直执行 argv, capture stdout 到 out(outsz)。
 * 返回 0 成功, 1 超时（子进程已被 SIGKILL）, -1 系统错误。
 * 成功/超时时 *rc = 退出码（超时为 -1）。
 *
 * 【2026-10-22 修复】原实现 `read()` + `waitpid(...,0)` 都没有超时，而
 * dispatch 是整段持 g_eng.mtx 的（main.c:1789→1847）——一条卡住的命令
 * 会让调用线程永久阻塞在锁内，此后所有请求（读内存、搜索、attach）
 * 全部排队，表现为「守护进程假死，但进程还在、端口还开着」。
 * 现在两条等待路径都带 deadline：读用 poll，收尸用 WNOHANG 轮询。 */
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

    uint64_t deadline = shell_now_ms() + SHELL_TIMEOUT_MS;
    size_t got = 0;
    int timed_out = 0;

    /* 读阶段：读满 out 之后继续**丢弃**输出，直到 EOF 或超时。
     * 不丢弃的话，管道写满会让子进程阻塞在 write 上，正常命令也会被误判超时。 */
    for (;;) {
        uint64_t now = shell_now_ms();
        if (now >= deadline) { timed_out = 1; break; }
        struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
        int pr = poll(&pfd, 1, (int)(deadline - now));
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) { timed_out = 1; break; }

        char discard[512];
        int room = (got < outsz - 1);
        char *dst = room ? out + got : discard;
        size_t cap = room ? (outsz - 1 - got) : sizeof(discard);
        ssize_t n = read(fds[0], dst, cap);
        if (n > 0) {
            if (room) got += (size_t)n;
            continue;
        }
        if (n == 0) break;                 /* EOF：子进程已关闭 stdout */
        if (errno == EINTR) continue;
        break;
    }
    out[got] = 0;
    close(fds[0]);

    if (timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);             /* 已 SIGKILL，立即返回，不会拖住 */
        *rc = -1;
        return 1;
    }

    /* 收尸阶段同样限时：子进程可能关了 stdout 却一直不退出。
     * 用 WNOHANG 轮询而不是阻塞 waitpid，超时则 SIGKILL 再收。 */
    int status = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0) {
            if (errno == EINTR) continue;
            *rc = -1;
            return -1;
        }
        if (shell_now_ms() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            *rc = -1;
            return 1;
        }
        struct timespec ts = { 0, (long)SHELL_REAP_TICK_MS * 1000000L };
        nanosleep(&ts, NULL);
    }
    *rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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
    int sx = shell_execvp(argv, out, sizeof(out), &rc);
    if (sx < 0) {
        send_err(fd, GGCMD_SHELL, seq, GGERR_INTERNAL, "exec failed");
        return;
    }
    if (sx == 1) {
        /* 超时：子进程已被 SIGKILL。把已捕获的部分输出附在错误信息里（截断），
         * 否则调用方只看到「超时」，无法区分「完全没输出（卡在系统调用）」
         * 与「输出到一半卡住」——这两种的排查方向完全不同。 */
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "shell timed out after %dms, killed (partial output: %.200s)",
                 SHELL_TIMEOUT_MS, out);
        send_err(fd, GGCMD_SHELL, seq, GGERR_TIMEOUT, msg);
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "output", out);
    cJSON_AddNumberToObject(r, "rc", rc);
    send_ok(fd, GGCMD_SHELL, seq, r);
    cJSON_Delete(r);
}