/* cmd_touch.c — 内核虚拟触摸注入（自 main.c 拆出，2026-10-23，Phase 2·⑥）
 *
 * 域内命令：GGCMD_GHOST_TAP / GGCMD_GHOST_SWIPE
 *
 * 这两条**不依赖 attach** —— 触摸是系统级输入事件，跟目标进程无关
 * （driver_touch_tap 走的是 rwproc33 的内核注入通道）。
 *
 * 一个已知缺口（schema 层没钳制）：坐标直接用 uint32 传给驱动，
 * 超出屏幕范围时由驱动那边决定行为。要在 schema 里加 max 约束的话，
 * 记得两端一起改（PC 端 schema.cpp + 这里的兜底判断）。
 */
#include "engine_internal.h"

#include <stdio.h>
#include <string.h>

void h_ghost_tap(int fd, uint32_t seq, cJSON *p) {
    cJSON *jx = cJSON_GetObjectItem(p, "x");
    cJSON *jy = cJSON_GetObjectItem(p, "y");
    if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) {
        send_err(fd, GGCMD_GHOST_TAP, seq, GGERR_BAD_REQUEST, "need x,y (display coords)");
        return;
    }
    uint32_t x = (uint32_t)jx->valuedouble;
    uint32_t y = (uint32_t)jy->valuedouble;
    uint32_t hold = 120;
    cJSON *jh = cJSON_GetObjectItem(p, "hold_ms");
    if (cJSON_IsNumber(jh)) hold = (uint32_t)jh->valuedouble;

    if (driver_touch_tap(&g_eng.drv, x, y, hold) != 0) {
        send_err(fd, GGCMD_GHOST_TAP, seq, GGERR_DEGRADED, g_eng.drv.last_error);
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "ok", 1);
    cJSON_AddNumberToObject(r, "x", x);
    cJSON_AddNumberToObject(r, "y", y);
    cJSON_AddNumberToObject(r, "hold_ms", hold);
    send_ok(fd, GGCMD_GHOST_TAP, seq, r);
    cJSON_Delete(r);
}

void h_ghost_swipe(int fd, uint32_t seq, cJSON *p) {
    cJSON *p1 = cJSON_GetObjectItem(p, "x1");
    cJSON *p2 = cJSON_GetObjectItem(p, "y1");
    cJSON *p3 = cJSON_GetObjectItem(p, "x2");
    cJSON *p4 = cJSON_GetObjectItem(p, "y2");
    if (!cJSON_IsNumber(p1) || !cJSON_IsNumber(p2) ||
        !cJSON_IsNumber(p3) || !cJSON_IsNumber(p4)) {
        send_err(fd, GGCMD_GHOST_SWIPE, seq, GGERR_BAD_REQUEST, "need x1,y1,x2,y2");
        return;
    }
    uint32_t ms = 300;
    cJSON *jm = cJSON_GetObjectItem(p, "ms");
    if (cJSON_IsNumber(jm)) ms = (uint32_t)jm->valuedouble;

    if (driver_touch_swipe(&g_eng.drv,
                           (uint32_t)p1->valuedouble, (uint32_t)p2->valuedouble,
                           (uint32_t)p3->valuedouble, (uint32_t)p4->valuedouble,
                           ms) != 0) {
        send_err(fd, GGCMD_GHOST_SWIPE, seq, GGERR_DEGRADED, g_eng.drv.last_error);
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "ok", 1);
    send_ok(fd, GGCMD_GHOST_SWIPE, seq, r);
    cJSON_Delete(r);
}
