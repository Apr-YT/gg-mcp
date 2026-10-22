#!/usr/bin/env python3
"""check_hwbp_detail.py — GET_HWBP_HIT_DETAIL 的 buf_size 护栏

背景（2026-10-22 真机踩到）：
    rwproc 的 33 字节请求头里，`buf_size` 是**内核侧决定回写多少**的字段；
    用户态 read() 的 count（本代码里叫 resp_cap）只决定缓冲区多大，不进协议头。

    `rwproc_call_ex(d, fd, cmd, p1, p2, p3, data, data_size, resp_cap, out, out_cap)`
    的第 8 个实参 data_size 才是写进 buf_size 的值。

    模块端 GET_HWBP_HIT_DETAIL 的实现是：
        size_t size = hdr->buf_size;
        if (size < sizeof(struct HWBP_HIT_ITEM)) return -EINVAL;

    所以 data_size 传 0 时，**每一条命中详情都会 EINVAL**，而上层的表现是
    「断点注册成功、watch_poll 永远 0 条」—— 看着像 perf 没触发，实际是协议头
    少填了一个字段。这个 bug 在真机上是靠设备端直连探针（hwbp_hits.c）才定位的。

本护栏断言：
    driver.c 里唯一一处 RWBP_CMD_GET_HWBP_HIT_DETAIL 调用，其 data_size 实参
    不能是字面量 0（必须是承载条目字节数的表达式）。

注意：护栏只钉「别再传 0」，不校验 resp_cap / out_cap 与 data_size 是否一致
（那需要跑协议，交给真机 hwbp_hits 工具）。

用法: python check_hwbp_detail.py <gg-root>
退出码: 0=通过, 1=违反, 2=参数/IO 错误
"""
import re
import sys
import os

CMD = "RWBP_CMD_GET_HWBP_HIT_DETAIL"

def main():
    if len(sys.argv) < 2:
        print("usage: check_hwbp_detail.py <gg-root>")
        return 2
    path = os.path.join(sys.argv[1], "agentd", "driver.c")
    try:
        src = open(path, encoding="utf-8", errors="replace").read()
    except OSError as e:
        print("[hwbp-detail-check] cannot read %s: %s" % (path, e))
        return 2

    # 只找调用点：向后定位到 ");" 截出整段实参，再按逗号切分。
    # 实参序 (d, fd, cmd, p1, p2, p3, data, data_size, resp_cap, out, out_cap)
    # -> cmd 的下标 +5 是 data_size。
    found = 0
    bad = []
    for m in re.finditer(re.escape(CMD), src):
        start = m.start()
        # 向前找最近的 rwproc_call_ex(
        back = src.rfind("rwproc_call_ex", 0, start)
        if back < 0:
            continue
        paren = src.find("(", back)
        if paren < 0 or paren > start:
            continue
        semi = src.find(");", start)
        if semi < 0:
            continue
        body = src[paren + 1:semi]
        args = [a.strip() for a in body.split(",")]
        idx = None
        for i, a in enumerate(args):
            if a == CMD:
                idx = i
                break
        if idx is None:
            continue
        found += 1
        data_size = args[idx + 5] if len(args) > idx + 5 else "<缺参>"
        norm = data_size.replace("(", "").replace(")", "").strip()
        if norm.lower() in ("0", "0u", "0ul", "0l", "null", "nullptr"):
            bad.append("data_size 传了 %s（buf_size 恒为 0 -> 模块返回 -EINVAL）"
                       % data_size)

    if found == 0:
        print("[hwbp-detail-check] FAIL: 找不到 %s 的调用点" % CMD)
        return 1
    if bad:
        for b in bad:
            print("[hwbp-detail-check] FAIL: %s" % b)
        print("[hwbp-detail-check] 修法: 传条目字节数（如 bytes），"
              "read() 的 count 不算数")
        return 1
    print("[hwbp-detail-check] OK (%d 处调用，data_size 均非字面量 0)" % found)
    return 0

if __name__ == "__main__":
    sys.exit(main())
