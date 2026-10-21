#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
cfm_coord_read.py —— 穿越火线手游(CFM, com.tencent.tmgp.cf) 坐标无痕读取器

设计前提（来自 docs/research/CF-ESP代码框架.md + 坐标加密对抗方法.md）：
  * CFM = Unity/il2cpp 引擎，坐标值是**明文 Vec3**，存于运行时提取的字段偏移，
    没有 DFM(三角洲/Unreal) 那种 shellcode 值加密 —— 无需 Unicorn 解密。
  * 读取走 gg 内核驱动(rwproc33/apr_dlkm)，经 MCP(http://127.0.0.1:8788/mcp)：
      - gg_attach 在当前 kernel-driver 模式仅登记 session_pid，**不注入**（见 agentd main.c h_attach），
        内核直读对 ACE/TPRT 完全不可见（TPRT 不 hook 读取路径、process_vm=0）。
      - 铁律：只读不写/不冻结；CFM 坐标读取不触发注入，安全。
  * 真正难点是「对象基址定位」(EntityStateSyncManager 实例)，见 --locate 模式。

字段偏移（CF-ESP代码框架.md §1.1/§1.2，已实锤；版本不符时改这里即可）：
  EntityStateSyncManager:
      m_localPawnPos @ 0x3c   (Vec3, 本地坐标)
      m_ThreadEntitys @ 0x18  (List<LogicalEntity> 引用数组)
  LogicalEntity(+PawnEntity 续排):
      Position  @ 0x24 (Vec3)
      Rotation  @ 0x30 (Vec3)
      Camp      @ 0x44 (u32)   —— PawnEntity 区
      IsAlive   @ 0x58 (u32)
  il2cpp 容器布局(标准):
      List   : klass@0 monitor@8 _items@0x10 _size@0x18 _version@0x1c
      Array  : klass@0 monitor@8 length@0x10 data@0x18
      元素 = 8B 引用指针

用法：
  python cfm_coord_read.py list                      # 列出进程，确认 CFM 在跑 + 拿 pid
  python cfm_coord_read.py attach --pid <PID>        # 登记会话(无注入)
  python cfm_coord_read.py read --essm <ADDR>        # 给定 ESSM 基址，读出全部实体坐标
  python cfm_coord_read.py locate --pid <PID>        # 浮点变化法定位 m_localPawnPos(只读,无字符串搜索)
仅授权安全研究/防御分析用途。
"""
import sys, json, struct, argparse, urllib.request

MCP_URL = "http://127.0.0.1:8788/mcp"
CFM_PKG = "com.tencent.tmgp.cf"

# ---- 字段偏移常量（版本不符改这里）----
ESSM_LOCAL_PAWN_POS = 0x3C
ESSM_THREAD_ENTITYS = 0x18
LE_POSITION          = 0x24
LE_ROTATION          = 0x30
PE_CAMP              = 0x44
PE_IS_ALIVE          = 0x58

class Mcp:
    def __init__(self, url=MCP_URL):
        self.url = url
        self._id = 0
        self._init()

    def _init(self):
        body = json.dumps({"jsonrpc":"2.0","id":0,"method":"initialize",
                           "params":{"protocolVersion":"2024-11-05","capabilities":{},
                                     "clientInfo":{"name":"cfm_coord","version":"1"}}}).encode()
        req = urllib.request.Request(self.url, data=body,
                                     headers={"Content-Type":"application/json"})
        urllib.request.urlopen(req, timeout=10).read()

    def call(self, tool, args=None):
        self._id += 1
        body = json.dumps({"jsonrpc":"2.0","id":self._id,"method":"tools/call",
                           "params":{"name":tool,"arguments":args or {}}}).encode()
        req = urllib.request.Request(self.url, data=body,
                                     headers={"Content-Type":"application/json"})
        raw = urllib.request.urlopen(req, timeout=30).read().decode()
        resp = json.loads(raw)
        if "error" in resp:
            raise RuntimeError(f"MCP error: {resp['error']}")
        # 解析 content[0].text -> JSON 字符串
        txt = resp["result"]["content"][0]["text"]
        try:
            return json.loads(txt)
        except Exception:
            return {"_raw": txt}

    # ---- 读原语 ----
    def read_qword(self, addr):
        r = self.call("gg_read", {"addr": hex(addr), "type": "qword"})
        return int(r.get("value", 0))

    def read_dword(self, addr):
        r = self.call("gg_read", {"addr": hex(addr), "type": "dword"})
        return int(r.get("value", 0))

    def read_float(self, addr):
        r = self.call("gg_read", {"addr": hex(addr), "type": "float"})
        return float(r.get("value", 0.0))

    def read_vec3(self, addr):
        r = self.call("gg_read_bytes", {"addr": hex(addr), "len": 12})
        hx = r.get("hex", "")
        if not hx or len(hx) < 24:
            return None
        b = bytes.fromhex(hx[:24])
        return struct.unpack("<fff", b)

    def list_processes(self, filt=None):
        return self.call("gg_list_processes", {"filter": filt} if filt else {})

    def attach(self, pid=None, name=None):
        a = {"pid": pid} if pid else {"name": name}
        return self.call("gg_attach", a)

    def detach(self):
        return self.call("gg_detach", {})

    def result_count(self):
        try:
            r = self.call("gg_result_count", {})
            # 模糊搜索用 fuzzy_candidates（位图候选数）；精确搜索用 count
            if r.get("fuzzy_active"):
                return r.get("fuzzy_candidates", 0)
            return r.get("count", 0)
        except Exception:
            return 0

    def get_results(self, offset=0, limit=200):
        return self.call("gg_get_results", {"offset": offset, "limit": limit})


def find_cfm(mcp):
    procs = mcp.list_processes(CFM_PKG) or []
    # gg_list_processes 返回结构: {processes:[{pid,name,comm,...}]}
    for p in procs.get("processes", procs if isinstance(procs, list) else []):
        nm = (p.get("name") or p.get("comm") or "")
        if CFM_PKG in nm or "cf" in nm.lower():
            return int(p.get("pid"))
    return None


def read_entities(mcp, essm):
    """读 ESSM -> 本地坐标 + 实体列表(坐标/阵营/存活)"""
    out = {"essm": hex(essm), "local_pos": None, "entities": []}
    # 本地坐标
    out["local_pos"] = mcp.read_vec3(essm + ESSM_LOCAL_PAWN_POS)
    # 实体列表 List<LogicalEntity>
    list_ptr = mcp.read_qword(essm + ESSM_THREAD_ENTITYS)
    if not list_ptr:
        return out
    items_ptr = mcp.read_qword(list_ptr + 0x10)
    size = mcp.read_dword(list_ptr + 0x18)
    if not items_ptr or size <= 0 or size > 4096:
        return out
    arr_len = mcp.read_dword(items_ptr + 0x10)
    n = min(size, arr_len)
    for i in range(n):
        ent = mcp.read_qword(items_ptr + 0x18 + i * 8)
        if not ent:
            continue
        pos = mcp.read_vec3(ent + LE_POSITION)
        rot = mcp.read_vec3(ent + LE_ROTATION)
        camp = mcp.read_dword(ent + PE_CAMP)
        alive = mcp.read_dword(ent + PE_IS_ALIVE)
        out["entities"].append({
            "idx": i, "ptr": hex(ent),
            "pos": [round(x, 2) for x in pos] if pos else None,
            "rot": [round(x, 2) for x in rot] if rot else None,
            "camp": camp, "alive": alive,
        })
    return out


def cmd_list(mcp):
    procs = mcp.list_processes() or {}
    arr = procs.get("processes", procs if isinstance(procs, list) else [])
    print(f"[processes] count={len(arr)}")
    for p in arr:
        print(f"  pid={p.get('pid')} name={p.get('name')} comm={p.get('comm')}")


def cmd_read(mcp, essm):
    pid = find_cfm(mcp)
    if not pid:
        print("[!] CFM 未运行 (com.tencent.tmgp.cf)，先启动游戏"); return
    print(f"[attach] pid={pid}")
    mcp.attach(pid=pid)
    data = read_entities(mcp, essm)
    print(json.dumps(data, ensure_ascii=False, indent=2))


def cmd_locate(mcp, pid):
    """浮点变化法定位 m_localPawnPos（只读，无字符串搜索，规避 08-18 封号动作）"""
    if not pid:
        pid = find_cfm(mcp)
    if not pid:
        print("[!] CFM 未运行"); return
    print(f"[attach] pid={pid}")
    mcp.attach(pid=pid)
    # 第一轮：模糊初始化（全区域 float）
    r0 = mcp.call("gg_search_unknown_init", {"type": "float", "region": "all"})
    print(f"[unknown_init] elements={r0.get('elements')}")
    print("[*] 请在游戏内移动一段距离（走路/跳），然后本脚本再跑: locate_refine --op changed")


def cmd_status(mcp):
    """查看当前搜索候选数（确认搜索会话还在）"""
    n = mcp.result_count()
    print(f"[status] candidates={n}")


def cmd_candidates(mcp, limit):
    """dump 当前候选地址（前 limit 个），便于挑出可能的 Vec3 位置读验证"""
    res = mcp.get_results(offset=0, limit=limit)
    addrs = res.get("results", [])
    print(f"[candidates] count_shown={len(addrs)} (total={mcp.result_count()})")
    for a in addrs[:limit]:
        addr = a.get("address") if isinstance(a, dict) else a
        print(f"  {addr}")


def cmd_locate_refine(mcp, pid, op):
    """第二轮：按值变化 refine（GG 标准模糊搜索流）。op=changed 保留移动后变化的地址。

    注意：agentd 的 session_pid 与搜索结果集是**常驻**的（跨 MCP 连接保留），
    因此本函数**不重复 attach**（h_attach 会 results_clear 清空候选，导致 refine 永远在空集上跑）。
    若结果集为空（agentd 重启/未 locate），提示用户先跑 locate。
    """
    n = mcp.result_count()
    if n <= 0:
        print("[!] 当前无搜索候选（可能 agentd 重启或还没 locate）。请先跑: locate --pid <PID>")
        return
    r = mcp.call("gg_search_refine", {"op": op})
    print(f"[refine op={op}] remaining={r.get('remaining')}")
    print("[*] 反复 移动→changed / 站定→unchanged 收敛到少量候选，再用 candidates 看地址、read 验证哪是本地坐标")


def main():
    ap = argparse.ArgumentParser(description="CFM 坐标无痕读取器")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    pa = sub.add_parser("read"); pa.add_argument("--essm", type=lambda s: int(s, 0), required=True)
    pl = sub.add_parser("locate"); pl.add_argument("--pid", type=int, default=None)
    pr = sub.add_parser("locate_refine"); pr.add_argument("--pid", type=int, default=None)
    pr.add_argument("--op", default="changed", choices=["changed","unchanged","increased","decreased"])
    pst = sub.add_parser("status")
    pc = sub.add_parser("candidates"); pc.add_argument("--limit", type=int, default=50)
    args = ap.parse_args()

    mcp = Mcp()
    if args.cmd == "list":
        cmd_list(mcp)
    elif args.cmd == "read":
        cmd_read(mcp, args.essm)
    elif args.cmd == "locate":
        cmd_locate(mcp, args.pid)
    elif args.cmd == "locate_refine":
        cmd_locate_refine(mcp, args.pid, args.op)
    elif args.cmd == "status":
        cmd_status(mcp)
    elif args.cmd == "candidates":
        cmd_candidates(mcp, args.limit)


if __name__ == "__main__":
    main()
