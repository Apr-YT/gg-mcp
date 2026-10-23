/* wire_auth.c — 握手鉴权与 wire 密钥管理（自 main.c 拆出，2026-10-23，Phase 1·③）
 *
 * 为什么单独拆：这是"谁能连上来"的唯一闸门，跟业务 handler 无关，
 * 但一改就容易出安全洞，所以给它一个独立文件方便审查。
 *
 * 设计要点（改之前必读）：
 *
 *   1. **没有密钥时不能放行。** agentd 监听 127.0.0.1，本机任意 App 都能连，
 *      等于把 root 级命令（shell / write / 加载 KO）交给任何拿到回环端口的进程。
 *      原实现"未配置即无条件成功"、只在日志打一行 WARN —— 客户端看握手是成功的，
 *      没人会去翻日志。现在未指定密钥就生成临时密钥。
 *
 *   2. **临时密钥落盘复用**（GG_EPHEMERAL_KEY_PATH，0600，与正式 key 分开）。
 *      否则每次重启 key 都变，客户端得跟着改。
 *
 *   3. **ct_equal 必须保持常量时间。** 按首个不同字节提前 return 会泄露密钥前缀，
 *      让猜解从 16^32 降到逐字符试探。这个函数故意保持 static —— 只给本文件用。
 *
 *   4. **绝不把密钥值写进日志**（对端的和本机的都不行）。失败只记录
 *      "key missing" / "key mismatch" 两种类型。
 *
 *   5. **空 key 分支是 fail-closed 兜底。** 正常流程下 g_wire_key 一定有值
 *      （main() 里 --wire-key / --wire-key-file / 临时密钥三选一，全失败就拒绝启动），
 *      留着这个分支是防止日后有人改坏启动顺序后退回"无条件放行"。
 */
#include "engine_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 共享密钥（hex 字符串）。来源优先级：--wire-key > --wire-key-file > 空。
 * 空串 = 不启用鉴权：设备上任何拿到 INTERNET 权限的本地 App 都能连
 * 127.0.0.1:<port> 直接调用 GGCMD_SHELL / WRITE / LOAD_GG（agentd 是 root）。
 * 因此启动时会打印醒目警告，正式部署必须配置。 */
char g_wire_key[65] = {0};

/* 常量时间比较：避免按首个不同字节提前返回、泄露密钥前缀。 */
static int ct_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return 0;
    unsigned char d = 0;
    for (size_t i = 0; i < la; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

/* ---------------- wire 鉴权密钥 ----------------
 *
 * 没有密钥时**不能放行**：agentd 监听 127.0.0.1:34127，本机任意 App 都能连，
 * 等于把 root 级命令（shell / write / 加载 KO）交给任何拿到回环端口的进程。
 * 原实现"未配置即无条件成功"，只在启动日志里打一行 WARN —— 从客户端看
 * 握手是成功的，没人会去翻日志。
 *
 * 现在：未指定 --wire-key / --wire-key-file 时，生成一个随机临时密钥。
 * 拒绝启动也是一种做法，但会让手工调试（不写部署脚本）直接起不来；
 * 生成临时密钥既保留可用性，又不再有"谁都能连"的窗口。
 *
 * 临时密钥落盘到 GG_EPHEMERAL_KEY_PATH（0600，与正式 key 分开），
 * 下次启动优先复用它 —— 否则每次重启 key 都变，客户端得跟着改。 */




int g_wire_key_ephemeral = 0;   /* 1 = 本次用的是自动生成的临时 key */

/* 读 nbytes 字节 /dev/urandom 并写成 hex。成功返回 0。
 * 用 O_RDONLY 直接 read 而不是 fopen：这条路径不该因为 stdio 缓冲引入额外状态。 */
int gen_random_hex(char *out, size_t outsz, size_t nbytes) {
    if (outsz < nbytes * 2 + 1) return -1;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    unsigned char rnd[GG_WIRE_KEY_BYTES];
    size_t need = nbytes, got = 0;
    if (need > sizeof(rnd)) { close(fd); return -1; }
    while (got < need) {
        ssize_t n = read(fd, rnd + got, need - got);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            close(fd);
            return -1;
        }
        got += (size_t)n;
    }
    close(fd);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < nbytes; i++) {
        out[i * 2]     = hex[rnd[i] >> 4];
        out[i * 2 + 1] = hex[rnd[i] & 0x0f];
    }
    out[nbytes * 2] = 0;
    return 0;
}

/* 读一个 key 文件的第一行（去掉换行）。成功返回 1，失败/空返回 0。 */
int read_key_file(const char *path, char *out, size_t outsz) {
    FILE *f = fopen(path, "re");
    if (!f) return 0;
    char line[128] = {0};
    int ok = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (!ok) return 0;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (n == 0 || n >= outsz) return 0;
    memcpy(out, line, n + 1);
    return 1;
}

/* 取临时密钥：优先复用落盘的那份，没有就生成并写盘（0600）。
 * 返回 0 成功；-1 表示连 /dev/urandom 都读不到（此时调用方必须拒绝启动）。 */
int load_or_create_ephemeral_key(char *out, size_t outsz) {
    if (read_key_file(GG_EPHEMERAL_KEY_PATH, out, outsz)) return 0;
    if (gen_random_hex(out, outsz, GG_WIRE_KEY_BYTES) != 0) return -1;

    int fd = open(GG_EPHEMERAL_KEY_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        size_t n = strlen(out);
        ssize_t w = write(fd, out, n);
        if (w == (ssize_t)n) { (void)write(fd, "\n", 1); }
        close(fd);
        (void)chmod(GG_EPHEMERAL_KEY_PATH, 0600);
    }
    /* 写盘失败不影响本次可用性：调用方会把 key 打到 stderr，操作者可手工取用 */
    return 0;
}

/* GGCMD_AUTH：握手。成功后本连接才放行其余命令（见 dispatch 的握手门）。
 *
 * 正常情况下 g_wire_key 一定有值：启动时载入 --wire-key / --wire-key-file，
 * 都没有就生成临时 key（见 load_or_create_ephemeral_key），生成失败则直接
 * 拒绝启动。所以下面这个空 key 分支只作为 fail-closed 兜底存在 —— 万一
 * 日后有人改坏启动顺序，也不能退回"无条件放行"。 */
void h_auth(int fd, uint32_t seq, cJSON *p, int *authed) {
    if (g_wire_key[0] == '\0') {
        log_msg("auth: rejected (server has no wire key configured)");
        send_err(fd, GGCMD_AUTH, seq, GGERR_PERM, "wire auth not configured");
        return;
    }
    cJSON *kj = cJSON_GetObjectItem(p, "key");
    const char *k = (kj && cJSON_IsString(kj)) ? kj->valuestring : NULL;
    if (!k || !ct_equal(k, g_wire_key)) {
        /* 只记录失败类型，绝不把密钥本身或对端提交值写进日志 */
        log_msg("auth: rejected (%s)", k ? "key mismatch" : "key missing");
        send_err(fd, GGCMD_AUTH, seq, GGERR_PERM, "wire auth failed");
        return;
    }
    *authed = 1;
    /* 认证通过 → 把读超时从"未认证的 10s"放宽到正常值（见 main.c 的
     * GG_CONN_PREAUTH_TIMEOUT_SEC）。不做这一步，长搜索会被 10s 超时打断。 */
    conn_arm_idle_timeout(fd);
    /* 客户端声明能重组分片（"frag":1）才给它发分片，否则保持单帧上限。
     * 这个协商是必须的：旧客户端收到 GGWF_MORE 帧会把第一片当完整响应解析。 */
    cJSON *fj = cJSON_GetObjectItem(p, "frag");
    int frag = cJSON_IsNumber(fj) ? (fj->valueint != 0) : cJSON_IsTrue(fj);
    if (frag) clients_set_frag(fd, 1);
    log_msg("auth: accepted (frag=%d)", frag);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "authed", 1);
    if (frag) cJSON_AddNumberToObject(r, "frag", 1);
    if (g_wire_key_ephemeral) cJSON_AddBoolToObject(r, "key_ephemeral", 1);
    send_ok(fd, GGCMD_AUTH, seq, r);
    cJSON_Delete(r);
}
