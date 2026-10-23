/* screen.c — 屏幕区域抓取（GGCMD_SCREENSHOT_ROI）
 *
 * 只做一件事：把屏幕上指定区域降采样成灰度回传给 PC，供「这块画面变了没」的差分判断。
 * 不做 OCR，不做整屏回传，也不决定"哪个数字是金币"——那是上层的事。
 *
 * ── 为什么不用 PNG ──────────────────────────────────────────────────────────
 * 设备上 `screencap -p` 实测 1.08s，其中 1.02s 是单核 PNG 压缩（1200x2608 RGBA）；
 * 而 raw 只要 0.18s。慢的是编码，不是截图。所以这里走 raw 流。
 *
 * ── 为什么只回传 ROI 而不是整屏 ────────────────────────────────────────────
 * raw 一帧 12.2MB，走 adb 要 750ms，且超过单帧 1MiB 上限（GGWM_MAX_PAYLOAD）。
 * 切出 ROI 再降采样后典型 1~2KB（hex 后 2~4KB），传输可以忽略。
 *
 * ── 为什么降采样 ──────────────────────────────────────────────────────────
 * 差分只关心「哪块区域整体变了」，不需要逐像素精度；降采样顺带压掉抗锯齿与
 * 闪烁噪声。scale 由调用方给，ROI 过大时这里自动上调，保证回传不超预算。
 */
#include "engine_internal.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define GG_SCREEN_MAX_PIXELS  8192   /* 回传灰度点数上限（hex 后 16KB，单帧 1MiB 内） */
#define GG_SCREEN_TIMEOUT_MS  5000   /* screencap 正常 180ms，5s 只是兜底 */
#define GG_SCREEN_BIN "/system/bin/screencap"

/* 读满 n 字节。screencap 的输出是管道、不能 seek，所以整帧都必须顺序读完
 * （实测 12.2MB 的 read 开销在几十 ms 量级，不值得为省它去改 screencap）。 */
static int screen_read_full(int fd, void *buf, size_t n, struct timespec deadline) {
    size_t got = 0;
    while (got < n) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long left_ms = (long)(deadline.tv_sec - now.tv_sec) * 1000L +
                       (deadline.tv_nsec - now.tv_nsec) / 1000000L;
        if (left_ms <= 0) return -1;
        struct pollfd pfd = { fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, (int)left_ms);
        if (pr <= 0) return -1;
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static uint32_t rd_u32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

void h_screenshot_roi(int fd, uint32_t seq, cJSON *p) {
    /* screencap 不碰目标进程，因此不要求 attach；但它是特权操作（读整屏像素），
     * 沿用与 shell 相同的思路：由 wire 的 AUTH 门控制，不再单独要 root。 */
    const cJSON *jx = cJSON_GetObjectItem(p, "x");
    const cJSON *jy = cJSON_GetObjectItem(p, "y");
    const cJSON *jw = cJSON_GetObjectItem(p, "w");
    const cJSON *jh = cJSON_GetObjectItem(p, "h");
    long rx = jx ? (long)jx->valuedouble : 0;
    long ry = jy ? (long)jy->valuedouble : 0;
    long rw = jw ? (long)jw->valuedouble : 0;
    long rh = jh ? (long)jh->valuedouble : 0;
    if (rw <= 0 || rh <= 0 || rx < 0 || ry < 0) {
        send_err(fd, GGCMD_SCREENSHOT_ROI, seq, GGERR_BAD_REQUEST, "x/y/w/h required (w,h > 0)");
        return;
    }
    int scale = 4;
    const cJSON *js = cJSON_GetObjectItem(p, "scale");
    if (js && js->valuedouble >= 1) scale = (int)js->valuedouble;
    if (scale > 64) scale = 64;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        send_err(fd, GGCMD_SCREENSHOT_ROI, seq, GGERR_INTERNAL, "pipe failed");
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        send_err(fd, GGCMD_SCREENSHOT_ROI, seq, GGERR_INTERNAL, "fork failed");
        return;
    }
    if (pid == 0) {
        /* 子进程只能用 async-signal-safe 调用（多线程 fork 的硬约束） */
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl(GG_SCREEN_BIN, "screencap", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += GG_SCREEN_TIMEOUT_MS / 1000;

    uint8_t hdr[12];
    if (screen_read_full(pipefd[0], hdr, sizeof(hdr), deadline) != 0) {
        close(pipefd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        send_err(fd, GGCMD_SCREENSHOT_ROI, seq, GGERR_TIMEOUT,
                 "screencap produced no header (binary missing or timeout)");
        return;
    }
    uint32_t sw_screen = rd_u32(hdr);
    uint32_t sh_screen = rd_u32(hdr + 4);
    /* hdr[8] 是 PixelFormat；实测 RGBA_8888(1) 与 RGBX 都是 4 字节/像素。
     * 若格式不是 4 字节（极少见），像素步长会错、切片全乱 —— 直接拒绝比返回垃圾好。 */
    uint32_t fmt = rd_u32(hdr + 8);
    if (sw_screen == 0 || sh_screen == 0 || sw_screen > 8192 || sh_screen > 8192 || fmt != 1) {
        close(pipefd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        char msg[96];
        snprintf(msg, sizeof(msg), "unsupported screenshot: %ux%u fmt=%u", sw_screen, sh_screen, fmt);
        send_err(fd, GGCMD_SCREENSHOT_ROI, seq, GGERR_INTERNAL, msg);
        return;
    }
    if ((uint32_t)rx + (uint32_t)rw > sw_screen || (uint32_t)ry + (uint32_t)rh > sh_screen) {
        close(pipefd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        char msg[128];
        snprintf(msg, sizeof(msg), "roi out of screen (screen %ux%u)", sw_screen, sh_screen);
        send_err(fd, GGCMD_SCREENSHOT_ROI, seq, GGERR_BAD_REQUEST, msg);
        return;
    }

    /* ROI 过大就抬 scale：宁可粗一点，也不能让回传把单帧撑爆 */
    while ((rw / scale) * (rh / scale) > GG_SCREEN_MAX_PIXELS) scale *= 2;
    if (scale > 64) scale = 64;
    size_t sw = (size_t)rw / (size_t)scale;
    size_t sh = (size_t)rh / (size_t)scale;
    if (sw == 0) sw = 1;
    if (sh == 0) sh = 1;

    uint8_t *line = malloc((size_t)sw_screen * 4u);
    uint8_t *out = malloc(sw * sh);
    uint32_t *acc = calloc(sw, sizeof(uint32_t));
    uint32_t *cnt = calloc(sw, sizeof(uint32_t));
    if (!line || !out || !acc || !cnt) {
        free(line); free(out); free(acc); free(cnt);
        close(pipefd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        send_err(fd, GGCMD_SCREENSHOT_ROI, seq, GGERR_INTERNAL, "oom");
        return;
    }

    size_t by = 0;
    int failed = 0;
    for (uint32_t row = 0; row < sh_screen && !failed; row++) {
        if (screen_read_full(pipefd[0], line, (size_t)sw_screen * 4u, deadline) != 0) {
            failed = 1;
            break;
        }
        if (row < (uint32_t)ry || row >= (uint32_t)(ry + rh)) continue;

        /* 累加这一行属于各块的像素亮度（BT.601 整数近似） */
        for (size_t bx = 0; bx < sw; bx++) {
            uint32_t px0 = (uint32_t)rx + (uint32_t)bx * (uint32_t)scale;
            uint32_t px1 = px0 + (uint32_t)scale;
            if (px1 > (uint32_t)(rx + rw)) px1 = (uint32_t)(rx + rw);
            for (uint32_t px = px0; px < px1; px++) {
                const uint8_t *q = line + (size_t)px * 4u;   /* RGBA */
                acc[bx] += ((uint32_t)q[0] * 77u + (uint32_t)q[1] * 151u + (uint32_t)q[2] * 28u) >> 8;
                cnt[bx]++;
            }
        }
        /* 块的下边界（或 ROI 末行）到齐就落一行灰度 */
        int row_close = (((row - (uint32_t)ry) % (uint32_t)scale) == (uint32_t)(scale - 1)) ||
                        (row == (uint32_t)(ry + rh) - 1u);
        if (row_close && by < sh) {
            for (size_t bx = 0; bx < sw; bx++) {
                out[by * sw + bx] = cnt[bx] ? (uint8_t)(acc[bx] / cnt[bx]) : 0;
                acc[bx] = 0;
                cnt[bx] = 0;
            }
            by++;
        }
    }
    free(line);
    close(pipefd[0]);
    kill(pid, SIGKILL);          /* 已读完就不等它自然退出，避免残留 */
    waitpid(pid, NULL, 0);

    if (failed || by != sh) {
        free(out); free(acc); free(cnt);
        send_err(fd, GGCMD_SCREENSHOT_ROI, seq, GGERR_TIMEOUT, "screencap interrupted");
        return;
    }
    free(acc);
    free(cnt);

    size_t npx = sw * sh;
    char *hex = malloc(npx * 2u + 1u);
    if (!hex) {
        free(out);
        send_err(fd, GGCMD_SCREENSHOT_ROI, seq, GGERR_INTERNAL, "oom");
        return;
    }
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < npx; i++) {
        hex[i * 2] = H[out[i] >> 4];
        hex[i * 2 + 1] = H[out[i] & 0xF];
    }
    hex[npx * 2] = '\0';
    free(out);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "x", (double)rx);
    cJSON_AddNumberToObject(r, "y", (double)ry);
    cJSON_AddNumberToObject(r, "w", (double)rw);
    cJSON_AddNumberToObject(r, "h", (double)rh);
    cJSON_AddNumberToObject(r, "sw", (double)sw);
    cJSON_AddNumberToObject(r, "sh", (double)sh);
    cJSON_AddNumberToObject(r, "scale", (double)scale);
    cJSON_AddStringToObject(r, "format", "gray");
    cJSON_AddStringToObject(r, "data", hex);
    send_ok(fd, GGCMD_SCREENSHOT_ROI, seq, r);
    cJSON_Delete(r);
    free(hex);
}
