#!/usr/bin/env python3
"""test_stdio_smoke.py - PC-side end-to-end smoke test over stdio transport.

Why this exists:
  unit tests cover pure functions; test_api.bat only proves the HTTP server
  answers /healthz. Neither exercises the JSON-RPC layer the way a real MCP
  client does (line-delimited JSON over stdin/stdout). This script drives the
  real executable and asserts on real responses.

Coverage (all runnable WITHOUT a device attached):
  1. initialize / tools/list shape
  2. JSON-RPC error layering: -32700 / -32600 / -32601
  3. unknown tool name does not crash the server
  4. invalid enum value is rejected with BAD_REQUEST (not silently defaulted)
  5. no-device path returns NO_DEVICE instead of hanging forever
  6. save_to rejects absolute paths / traversal

Exit code 0 = all pass, 1 = at least one failure.
ASCII-only output on purpose: Windows console may be GBK.
"""

import json
import os
import queue
import subprocess
import sys
import threading

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXE = os.path.join(ROOT, "build", "pc", "gg-ghost-mcp.exe")
CFG = os.path.join(ROOT, "configs", "gg-ghost.json")

# 测试隔离（2026-10-21 审查 F-16）：
# 用例 5 断言"无设备时必须返回错误而不是挂住"，但它原先直接用仓库配置。
# 只要本机 adb 有设备、forward 已建立、agentd 在跑，gg_list_processes 就会成功，
# 这条断言随即假失败 —— 实测 test_all 在有设备在线时直接红。
# 因此这里生成一份临时配置，把 agent_port 指到 discard 端口（必然无人监听），
# 让"无设备"路径变成确定性行为，而不是取决于机器的外部状态。
ISOLATED_CFG = os.path.join(ROOT, "build", "pc", "_smoke_isolated.json")


def _write_isolated_cfg():
    try:
        with open(CFG, "r", encoding="utf-8") as f:
            cfg = json.load(f)
    except Exception:  # 配置缺失/损坏时用最小集合，冒烟测试不依赖它
        cfg = {}
    cfg["transport"] = "stdio"
    cfg["agent_port"] = 9   # discard：connect 必然被拒，走 NO_DEVICE 分支
    os.makedirs(os.path.dirname(ISOLATED_CFG), exist_ok=True)
    with open(ISOLATED_CFG, "w", encoding="utf-8") as f:
        json.dump(cfg, f)
    return ISOLATED_CFG

passed = 0
failed = 0
failures = []


def check(cond, msg, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("ok  : %s" % msg)
    else:
        failed += 1
        failures.append(msg)
        print("FAIL: %s%s" % (msg, (" -> " + detail) if detail else ""))


class Client:
    """Drives the server: one JSON per line in, one JSON per line out."""

    def __init__(self):
        isolated = _write_isolated_cfg()
        self.proc = subprocess.Popen(
            # configs/gg-ghost.json 默认 transport=http，必须显式覆盖，
            # 否则 server 去监听 8788 而不是读 stdin（表现为本脚本永远收不到响应）。
            # 同时改用隔离配置（agent_port=9），让"无设备"用例不依赖外部设备状态。
            [EXE, "--config", isolated, "--transport", "stdio"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            encoding="utf-8",
            errors="replace",
        )
        self.out_q = queue.Queue()
        self.err_lines = []
        threading.Thread(target=self._pump_stdout, daemon=True).start()
        threading.Thread(target=self._pump_stderr, daemon=True).start()

    def _pump_stdout(self):
        for line in self.proc.stdout:
            self.out_q.put(line)
        self.out_q.put(None)

    def _pump_stderr(self):
        # Must drain stderr or the pipe fills up and the server blocks.
        for line in self.proc.stderr:
            self.err_lines.append(line.rstrip("\n"))

    def send(self, obj):
        self.proc.stdin.write(json.dumps(obj, separators=(",", ":")) + "\n")
        self.proc.stdin.flush()

    def send_raw(self, text):
        self.proc.stdin.write(text + "\n")
        self.proc.stdin.flush()

    def recv(self, timeout=15):
        line = self.out_q.get(timeout=timeout)
        if line is None:
            raise RuntimeError("server closed stdout")
        return json.loads(line)

    def call(self, req_id, method, params=None, timeout=15):
        req = {"jsonrpc": "2.0", "id": req_id, "method": method}
        if params is not None:
            req["params"] = params
        self.send(req)
        # Skip notifications (no id) and stale ids while waiting for ours.
        for _ in range(20):
            resp = self.recv(timeout=timeout)
            if resp.get("id") == req_id:
                return resp
        raise RuntimeError("no response for id=%s" % req_id)

    def tool(self, req_id, name, args=None, timeout=15):
        return self.call(req_id, "tools/call", {"name": name, "arguments": args or {}}, timeout)

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def err_code(resp):
    return (resp.get("error") or {}).get("code")


def tool_text(resp):
    """Flatten a tools/call result into plain text for substring assertions."""
    res = resp.get("result") or {}
    parts = []
    for item in res.get("content") or []:
        if isinstance(item, dict) and item.get("type") == "text":
            parts.append(item.get("text") or "")
    if not parts and "error" in resp:
        parts.append(json.dumps(resp["error"], ensure_ascii=False))
    return "\n".join(parts)


def main():
    if not os.path.exists(EXE):
        print("[FAIL] %s not found - run scripts\\build_pc.bat first" % EXE)
        return 1
    if not os.path.exists(CFG):
        print("[FAIL] %s not found" % CFG)
        return 1

    print("=== GG-MCP stdio end-to-end smoke ===")
    print("exe: %s" % EXE)
    print()

    c = Client()
    try:
        # ---- 1. handshake shape -------------------------------------------
        r = c.call(1, "initialize", {"protocolVersion": "2024-11-05", "capabilities": {}})
        check("result" in r and "error" not in r, "initialize returns result")
        info = (r.get("result") or {}).get("serverInfo") or {}
        check(bool(info.get("name")), "initialize has serverInfo.name", json.dumps(info)[:120])

        r = c.call(2, "tools/list", {})
        tools = (r.get("result") or {}).get("tools") or []
        names = [t.get("name") for t in tools if isinstance(t, dict)]
        check(len(names) > 0, "tools/list non-empty (count=%d)" % len(names))
        check(len(names) == len(set(names)), "tools/list has no duplicate names")
        for required in ("gg_read", "gg_search", "gg_attach"):
            check(required in names, "tool present: %s" % required)

        # ---- 2. JSON-RPC error layering -----------------------------------
        r = c.call(3, "no/such/method", {})
        check(err_code(r) == -32601, "unknown method -> -32601 MethodNotFound",
              "got code=%s" % err_code(r))

        r = c.call(4, "tools/call", None)  # missing params
        check(err_code(r) in (-32602, -32600),
              "tools/call without params -> -32602/-32600", "got code=%s" % err_code(r))

        r = c.call(5, "tools/call", {"name": "gg_no_such_tool", "arguments": {}})
        check(err_code(r) is not None or (r.get("result") or {}).get("isError") is True,
              "unknown tool name -> error, no crash",
              "resp=%s" % json.dumps(r)[:160])

        # malformed JSON: server must answer -32700 and keep serving.
        c.send_raw("{this is not json")
        r = c.recv()
        check(err_code(r) == -32700, "malformed JSON -> -32700 ParseError",
              "got code=%s" % err_code(r))
        r = c.call(6, "tools/list", {})
        check("result" in r, "server still alive after malformed input")

        # ---- 3. argument validation (no device needed) --------------------
        r = c.tool(7, "gg_read", {"addr": "0x1000", "len": 4, "type": "dwordd"})
        txt = tool_text(r)
        check("dwordd" in txt or "invalid" in txt.lower() or "type" in txt.lower(),
              "invalid enum type rejected (not silently defaulted)", txt[:120])
        check("0x1000" not in txt or "dwordd" in txt,
              "invalid type does not fall through to a real read", txt[:120])

        r = c.tool(8, "gg_search", {"value": "100", "type": "notatype"})
        txt = tool_text(r)
        check("notatype" in txt or "invalid" in txt.lower(),
              "gg_search invalid type rejected", txt[:120])

        # ---- 4. save_to path boundary -------------------------------------
        # save_to 是 gg_read_bytes 的参数；路径校验发生在 device_call 之前，
        # 所以无设备也能验证边界。
        r = c.tool(9, "gg_read_bytes",
                   {"addr": "0x1000", "len": 16, "save_to": "../../pwned.bin"})
        txt = tool_text(r)
        check(("save_to" in txt) and (("reject" in txt.lower()) or ("export_dir" in txt)),
              "save_to traversal rejected", txt[:140])

        # ---- 5. no-device behavior: must answer, not hang ----------------
        r = c.tool(10, "gg_list_processes", {}, timeout=25)
        txt = tool_text(r)
        res = r.get("result") or {}
        no_dev = ("NO_DEVICE" in txt.upper()) or ("no device" in txt.lower()) \
                 or res.get("isError") is True
        check(no_dev, "no-device call returns an error (does not hang)", txt[:140])

        # server must remain usable after a failed device call.
        r = c.call(11, "tools/list", {})
        check("result" in r, "server still alive after no-device call")
    except Exception as exc:  # noqa: BLE001 - report, do not mask
        failed_global = True
        print("FAIL: unexpected exception: %r" % (exc,))
        print("--- server stderr tail ---")
        for ln in c.err_lines[-15:]:
            print(ln)
        return 1
    finally:
        c.close()

    print()
    print("== %d passed, %d failed ==" % (passed, failed))
    if failed:
        for f in failures:
            print("  - %s" % f)
        return 1
    print("== ALL PASS ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())
