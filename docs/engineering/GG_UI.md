# GG-UI — PC 侧内存搜索界面

一个用 **imgui + Win32 + D3D11** 写的轻量搜索前端。目标是"打开就能搜"，
不依赖浏览器、不装运行时、不点一串 MCP 工具。

```
build\pc\gg-ui.exe                       # 交互使用（启动即自动连接）
build\pc\gg-ui.exe --pid 5462            # 启动即 attach 指定进程
build\pc\gg-ui.exe --pid 5462 --search --value 100 --type dword
build\pc\gg-ui.exe --pid 5462 --search --value 100 --type dword --page 3   # 直接看第 4 页
```

## 为什么单独做一个 UI

现有的 `gg-ghost-mcp.exe` 是给 AI 客户端用的（MCP over stdio/http），
手动调工具要拼 JSON、看返回。搜索是**高频、强交互**的操作：
改个数、再搜、看结果、冻结、解冻 —— 用对话式接口做这些很别扭。

## 不重复实现协议

UI **不走 MCP 传输层**，而是直接链接 `server/session.cpp`，调
`Session::call_tool(name, args)` —— 和 MCP 客户端走同一条路径：

```
gg-ui.exe ──► Session::call_tool("gg_search", {...})
                    │
                    └─► AgentClient (wire) ──adb forward──► agentd ──► KO
```

好处：协议改动、新工具、参数校验全部自动继承，UI 侧只关心"怎么摆界面"。

## 构建

```bat
scripts\build_ui.bat
```

- imgui 已 vendored 到 `third_party\imgui`（**v1.91.9**，裁掉了 examples/docs 和
  除 win32/dx11 外的所有 backend，约 3MB）。
- 字体在构建时从 `C:\Windows\Fonts\msyh.ttc` 复制到 `build\`（19MB，**不入库**）。
  运行期也会再找系统字体兜底。
- 产物静态链接（`-static -static-libgcc -static-libstdc++`），运行时只依赖系统 DLL：
  `d3d11 / d3dcompiler_47 / dwmapi / gdi32 / kernel32 / shell32 / user32 / ws2_32`。

## 界面

```
┌ 顶栏: GG-UI | 连接/重连 | ● 驱动状态 | 目标: 进程名(pid) | AI 联动 开 | 刷新 ┐
├──────────────┬──────────────────────────────────────────────────────┤
│ 进程列表      │ 值 [100] 类型[dword] 区域[all] [精确搜索][区间搜索]    │
│ (可过滤)      │ [模糊首次扫描] 然后: [变][不变][增][减] [撤销][清空]   │
│              │ 选中: 0x... [写入值] [写入][冻结][解冻][读值]          │
│              ├──────────────────────────────────────────────────────┤
│              │ 结果 N 条 | 第 X - Y  [< 上页] [下页 >]  …工具按钮    │
│              │ 地址 | 值（十进制） | hex      ← 单击选中/双击读值     │
├──────────────┴──────────────────────────────────────────────────────┤
│ 日志（最近 300 条）                                                   │
└─────────────────────────────────────────────────────────────────────┘
```

## 与 AI 联动（2026-10-23）

### 这件事"几乎不用做"

agentd 的 `g_eng` 是全局单例：UI 和 MCP 服务器各开一条 TCP 连接，**看到的是
同一个 `session_pid` 和同一份 `results`**。所以"AI 搜完 UI 里能不能看到"
从来不是数据问题。缺的只是：

- AI 不知道**人**刚才手动搜了什么值、选中了哪一行
- 人不知道 **AI** 改了结果集

### 两条通道

实现集中在 `server/shared_ctx.h`（header-only，两个构建脚本都不用改），
文件在 `build/gg-shared/` 下（运行时产物，已 ignore）：

| 文件 | 谁写 | 谁读 | 内容 |
|---|---|---|---|
| `context.json` | gg-ui（attach / 搜索 / 选中 + 400ms 心跳） | AI 调 `gg_context` | 目标进程、搜索值/类型/区域/**模式**、结果数、选中行、最近动作、日志尾部 |
| `cmd.json` | AI 调 `gg_ui_command` | gg-ui（200ms 轮询） | `{id, op, args}`，执行后删文件并写 `cmd_result.json` |

用文件而不是本地 socket，理由：**崩溃安全**（进程挂了文件还在，能看到人最后在
干什么）、零依赖（不用手写 HTTP）、可调试（`cat` 一下就知道谁写了什么）。
代价是轮询，实测开销可忽略。

`search.mode`（`exact / range / fuzzy / none`）是关键字段：`value` 是输入框原文，
`range` 模式下它是 `"90-110"` 这样的区间写法，AI 不知道 mode 会把它当字面量。

### 场景一：手搜完，问 AI

在 UI 里搜 `100`、点中某一行，切到对话说"帮我看看这个地址附近是什么结构"。
AI 调 `gg_context` 就能拿到 `search.value=100 / type=dword / selected.addr=0x...`，
不用反问你搜的什么。

工具描述里已经写清触发时机（"用户提到'我刚搜的'、'这个地址'时先调它"）。

### 场景二：AI 搜完，看 UI

AI 直接调 `gg_search` 时，UI 每 900ms 比对一次设备侧 `result_count`，发现变了就
自动刷新表格，并在搜索区下方提示 `AI: 结果集被外部改动（N → M），已同步`。
AI 在后台干活时人的窗口自动跟上，不用手动点"重新拉取"。

判"是谁改的"靠本地基准 `total_seen`：本地每次刷新后同步它，所以只有**外部**
改动才会触发刷新，不会自己刷自己。

### 场景三：让 AI 驱动窗口

```
gg_ui_command(op="search",  args={"value":"200","type":"dword"})
gg_ui_command(op="attach",  args={"pid":5409})
gg_ui_command(op="select",  args={"addr":"0x2d5ff08"})
```

支持的 op：`attach / search / search_range / fuzzy_init / refine / select /
read / export / refresh`。

**故意不含 write / freeze**：改内存走 `gg_write` 那条有 safe_mode 拦截和审计的
路径，不开第二条。

执行结果同步返回（UI 200ms 轮询，正常 1 秒内回执）。UI 没开时**直接报错**，
不会把命令埋进队列让下次启动的 UI 误执行；即使写进去了，超过 30 秒的指令
UI 也会丢弃。

### 边界

- **UI 关掉后 AI 仍能读到最后状态**，但 `running=false`。判据是 `closed` 标记
  （正常退出时写）和 `ts` 新鲜度（3 秒）。这是刻意的：知道"人最后在搜 500"
  通常比"什么都不知道"有用。**强杀**（任务管理器结束进程）不会写 `closed`，
  要等 3 秒超时。
- **两个进程的工作目录必须一致**（`build/gg-shared/` 是相对路径）。
  不一致时设环境变量 `GG_SHARED_DIR` 指到同一个绝对路径。
- 顶栏点 `AI 联动 开` 可以关掉共享（会删 `context.json`）。
- 只支持单实例：开两个 gg-ui 会互相覆盖 `context.json`。

### 改动记录

- `server/shared_ctx.h` — 新增（原子写：tmp + rename，中途崩不留半个 JSON）
- `server/session.cpp` — `gg_context` / `gg_ui_command` 两个工具
- `server/mcp/schema.cpp` — 对应 schema（总数 44 → 46）
- `tools/gg-ui/main.cpp` — `ui_publish` / `ui_run_cmd` / `ui_tick`

### 结果分页

搜索常出几万条，但要看的一般只在某一段地址范围里。分页比"调大显示上限再卡一下"
实用：默认 1000 条/页，`< 上页` / `下页 >` 直接换 offset 重拉。

- 所有"重新搜索"的入口（attach / 精确 / 区间 / 模糊首帧 / 清空）都会把
  `page_offset` 归零 —— 否则会停在第 N 页看新结果，看起来像"搜了个空的"
- `refresh_results` 开头会把 offset 夹回合法范围：结果数变小（refine / undo）
  时自动落到最后一页，不会拉到空页
- `--page N` 走的是同一条路径，方便脚本验证（实测 `--page 100` 会自动夹到
  offset=9000、返回最后 832 条）

## 几个坑（改这个 UI 前先看）

1. **imgui 版本必须用稳定 tag**。先用 master（1.93.0 WIP）时，字体加进去了、
   `nfonts=1` 也正常，但界面中文全是方块 —— WIP 分支的 font system 在改。
   换 v1.91.9 后立刻正常。
2. **字形范围用 `ImFontGlyphRangesBuilder` 手动拼**（Default + 常用 2500 字 +
   界面文字）。直接把自己写的字符表传给 `AddFontFromFileTTF` 一定漏字
   （第一版就漏了"模糊/首次/进程"里的字）。
3. **字体文件放 `build/`**。本机实测直接加载 `C:\Windows\Fonts\msyh.ttc`
   会失败，复制一份到项目内就正常。
4. **窗口标题带调试信息**（`GG-UI - font=... nfonts=N`）。字体出问题时
   截图就能看出是"没加载"还是"加载了但 glyph 没烘进去"，省得猜。
5. 搜索是**同步**调用（全量约 0.2s、模糊首帧约 0.3s），会卡一下界面。
   真要异步就把操作丢进线程 + 结果队列，别在 UI 线程里加 sleep 等。

## 已知限制

- 结果分页是"一次拉一页"（默认 1000 条/页），没有预取下一页。设备端
  `gg_get_results` 每次要重读值，翻页约 30~80ms，肉眼可接受。
  分页只会因为"结果变少"（refine / undo / 换搜索）把 offset 夹回最后一页，
  不会自动跟随结果增删。
- 没有 watch/断点面板（这些是低频操作，仍走 MCP）。
- 进程列表一次拉全量（约 900+ 条），靠过滤框缩小；没做懒加载。
