# AGENTS.server.md — PC server / wire 协议 / UI（AGENTS.md 分册）

> 2026-11-04 从 AGENTS.md 拆出（原 50 条/963 行，见 BACKLOG P1-15）。
> **编号沿用主文件历史顺序，不重排**（条目间互引"见第 N 条"依赖原编号）。
> 跨文件引用请回主文件《已知债索引》查条目所在分册。

2. **wire 协议的三个遗留**：XOR seed 固定 `0xA5`（`shared/wire_protocol.h`）；serial
   硬编码在 config；分片虽有（第 18 条）但上限是硬的 —— 单帧
   `GGWM_MAX_PAYLOAD` 1MiB、重组 `GGWM_MAX_REASSEMBLED` 8MiB。
   - `gg_get_results` 上限 10000 条时约 510KB；分片后单帧上限不再是瓶颈，
     **但上调 limit 前先看 8MiB 重组上限**，超了会被 `read_frame`
     （`server/device/wire.cpp:286`）直接判非法。

10. GG-U4（无 key 即放行）已修：未指定 `--wire-key`/`--wire-key-file` 时生成
   16 字节临时密钥，落盘 `/data/adb/GG-Ghost/wire.key.ephemeral`（0600）并复用，
   stderr 打印；`/dev/urandom` 不可读才拒绝启动。AUTH 的空 key 分支改为拒绝。
   **手工调试须知**：不传 key 启动时，客户端要改用启动日志里打印的临时 key
   （或在设备上读那个 .ephemeral 文件）。正常部署不受影响。
   **另一个坑**：`start_agentd.sh start` 在端口被别的实例占用时会**误报"已启动"**
   （只按 pidfile 判断存活）。手工 `setsid` 起过实例后一定要先 `kill $(pidof gdm)`。

11. **大响应分片已实现（GG-U5）**，规则见第 18 条。调分片/超限分支时注意：
   现在没有可构造的超限响应，要验证得临时把 `GGWM_MAX_PAYLOAD` 改小**只重编设备端**
   （PC 端不用动），测完立即恢复。

15. **agentd 重启后 PC 端第一条请求会自动重连重放**（2026-10-22 修）。判据是
   「整帧是否发完」：`wr<0` 才重放，`wr==0` 绝不重放（对端可能已执行）。
   注意实测里失败通常是 `wr=-1`（帧头塞进缓冲成功、payload 阶段断），
   不是"零字节"，所以不能只判零字节。
   **调试 wire 层**：`GG_LOG_LEVEL=1` 才出日志（默认 2），且要重定向到文件，
   管道给 tail 看不到实时输出。

18. **大响应分片已实现**（`GGWF_MORE`，2026-10-22）。规则：
   - 客户端 AUTH 带 `"frag":1` 才会收到分片；旧客户端不变（超限仍 `GGERR_CAP`）
   - 同一响应所有分片共用 seq，最后一片不带 MORE
   - 每片 ≤ `GGWM_MAX_PAYLOAD`，重组上限 `GGWM_MAX_REASSEMBLED`（8MiB）
   **改这块极易踩的两点**：① XOR 必须逐片独立做（整体加密后切会让第二片起
   偏移全错）；② 整组分片要在同一次发送锁持有期间发完（中途插入别的响应会
   让接收端按 seq 拼错）。

25. **AI-Root 侧（AR）审计项已全部关闭**（2026-10-22 夜）。改 policy / consent /
   config 前必读这三条实测结论：
   - **自保护路径判定是两段式**：词法不命中后再用 `GetFinalPathNameByHandleW`
     解析真实路径。别把第二段删掉当"性能优化"——它挡的正是 8.3 短名
     （`PROGRA~1`）、junction、尾随点空格。解析结果若是 `C:\$Extend\$Deleted\*`
     必须丢弃（那是"已删除但仍有句柄"的返回值，拿去比对会得出假的"不命中"）。
   - **Windows 上 delete / rename 不能当并发抢占点**。多线程可以各自打开同一文件
     句柄（默认共享删除），对同一 file object 的删除/改名**每个调用都返回成功** ——
     实测 8 线程并发 `remove_file` 出现 2 个成功、`rename` 出现 4 个。要用
     `dwShareMode = 0` 独占打开 + `SetFileInformationByHandle(FileDispositionInfo)`。
   - **`default_autonomy` 允许解释器 = 允许任意代码执行**，这是刻意取舍。只把
     「即时执行外部内容」降为 Confirm（`npx`/`npm run`/`pip install`/`cargo install`/
     `go run`/`git -c`/`dotnet run`）。**改动只影响新生成的默认策略**，已部署的
     `policy.toml` 不会自动变。

35. **PC 侧搜索界面 `gg-ui`（imgui + Win32 + D3D11）**（2026-10-23）。

   ```
   scripts\build_ui.bat            # 产物 build\pc\gg-ui.exe（静态链接，只有系统 DLL 依赖）
   build\pc\gg-ui.exe --pid 5462 --search --value 100 --type dword
   ```

   **刻意不走 MCP 传输层**：UI 直接链接 `server/session.cpp`，调
   `Session::call_tool(name, args)`。协议、参数校验、新工具全部自动继承 ——
   再加一个前端不用碰 wire。改 UI 时只要认 `call_tool` 这一个入口。

   ### 三个坑（都踩过，别再踩）

   1. **imgui 必须用稳定 tag**（现在 vendored 的是 **v1.91.9**）。
      先用 master（当时是 1.93.0 WIP）：字体明明加载成功（标题栏
      `font=build\msyh.ttc nfonts=1`），界面中文却全是方块 ——
      那一版的 font system 正在改。换 1.91.9 立刻正常。
   2. **字形范围用 `ImFontGlyphRangesBuilder` 手动拼**（Default + 常用 2500 字）。
      只传自己列的字符表一定漏字（第一版漏了"模糊/首次/进程"，界面上是方块）。
   3. **字体文件放 `build/`**：本机实测直接从 `C:\Windows\Fonts\msyh.ttc`
      加载会失败，复制一份到项目内就正常。`build_ui.bat` 会自动复制
      （19MB，**不入库**）。

   ### 调试技巧

   窗口标题带 `font=<实际加载的路径> nfonts=<字体数>`。字体出问题时截个图就能
   区分"没加载"和"加载了但 glyph 没烘进去"，不用加日志、不用猜。

   ### 现状

   已实测：启动自动连接 → 913 个进程列表 → attach systemui →
   搜索 dword=100 得 23168 条 → 表格显示前 1000 条。中文全正常。

   已知限制：结果不分页（只显示前 N 条）、没有 watch/断点面板（低频操作仍走 MCP）、
   进程列表一次拉全量靠过滤框缩小。搜索是同步调用（全量 0.2s / 模糊首帧 0.3s），
   会短暂卡界面 —— 要异步就把操作丢线程，别在 UI 线程里 sleep 等。

36. **UI ↔ AI 联动：共享工作上下文**（2026-10-23，用户要求"手动搜索的值能与 AI 联动"）。

   ### 先纠正一个容易搞错的认知

   agentd 的 `g_eng` 是**全局单例**：UI 和 MCP 服务器各开一条 TCP 连接时，
   `session_pid` 和 `results` **本来就是同一份**。所以"数据同步"从一开始就存在。

   推论（重要）：**任何一方 attach 都会改掉全局 session_pid**。AI 调
   `gg_ui_command op=attach` 之后，AI 自己的会话也变成那个进程了。
   两条连接"各看各的进程"不支持，也不打算支持。

   真正缺的是"对方在干什么"，补了三个东西：

   | 能力 | 工具/机制 | 方向 |
   |---|---|---|
   | 读人的上下文 | `gg_context` | AI ← UI |
   | 驱动窗口 | `gg_ui_command`（attach/search/refine/select/read/export/refresh） | AI → UI |
   | 自动跟随 | UI 每 900ms 比对 `result_count` | AI → UI（被动） |

   `gg_ui_command` **故意不含 write / freeze**：改内存走 `gg_write` 那条有
   safe_mode 拦截和审计的路径，不开第二条。

   ### 通道

   `build/gg-shared/{context,cmd,cmd_result}.json`，实现 `server/shared_ctx.h`
   （header-only，两个构建脚本都不用改）。用文件不用本地 socket：崩溃安全
   （进程挂了文件还在）、零依赖、`cat` 一下就知道谁写了什么。原子写是
   tmp + rename，中途崩不会留半个 JSON 把读的一方卡死。

   ### 四个实现上的坑

   1. **`search.mode` 必须传**：`value` 是输入框原文，range 模式下它是
      `"90-110"`。不标模式，AI 会把它当字面量。
   2. **`ui_alive` 不能只看 ts**：正常退出写 `closed:true`，但**强杀不会**
      （任务管理器结束进程），所以还要 ts 3 秒过期兜底。两条都要留。
   3. **自动跟随要用本地基准区分"谁改的"**：记 `total_seen`，本地每次刷新后
      同步它。否则自己搜完会立刻触发一轮无意义刷新。
   4. **UI 没开时 `gg_ui_command` 必须直接报错**，不能把命令埋进队列 ——
      否则用户下次开 UI 会执行一条几十分钟前的 attach。UI 侧也校验 30 秒过期。

   ### 顺带修掉一个上次遗留的单测回归

   `agentd/search.c` 的 pagemap 代码用了 `pread` / `O_CLOEXEC`，Windows 主机
   编不过 —— 上一轮加完只跑了 itest 和真机验证，**没跑 `run_tests`**。
   现在用 `#if defined(__linux__)` 包住，非 Linux 给"pagemap 不可用"的桩：
   那正是设备上打不开 pagemap 的真实分支，所以单测跑的是 fallback 而不是
   假实现（真机的 present 位判断只能靠 itest + 集合比对验证，见第 30 条）。
   改完真机复测全量搜索 0.16~0.18s，pagemap 仍生效；`run_tests` 全绿，
   工具数 44 → 46（dispatch 护栏自动校验通过）。

39. **wire 小帧 40ms 陷阱：Nagle + delayed ACK（2026-10-23 靶场定案）**

   - **现象**：单点 `READ` 稳定 **40.0ms**，而 64KB `READ_BYTES` 只要 19ms
     —— 数据量小的反而慢
   - **判据**：延迟与数据量无关、抖动 < 0.2ms ⇒ 是定时器不是负载
   - **根因**：`send_frame` 两次 `send`（先 20B header、再 payload）。小 payload 时
     第二个包卡在"前一个包未确认"状态，触发 Nagle；对端 TCP **delayed ACK 正好 40ms**。
     大响应含满 MSS 段，不受 Nagle 限制，所以更快
   - **修法两件套（缺一个仍退化）**：`send_hdr_payload()` 一次 send 发完 header+payload
     ＋ accept 后 `setsockopt(TCP_NODELAY)`（NDK 的 `arpa/inet.h` 不带，要 `<netinet/tcp.h>`）
   - **结果**：READ 40.0→0.44ms、WRITE 35.4→0.46ms、MCP 路径 42.9→2.25ms
   - **回归探针**：`python tests/integration/lat_probe.py`（直连 34127，绕过 HTTP 层）
   - **教训**：wire 层任何"分两次写"的东西（header/body 分开、分片、chunked）
     都要先考虑 Nagle；量延迟一律直连测，HTTP + PowerShell 会把 ms 级差异埋掉

48. **wire 断线重连语义修复：AUTH 期间断连 ≠ 鉴权被拒（2026-11-04，已单测实证）**

   P0-6（ensure_connected_retry）收口时留下的尾巴：test_wire_io 用例 d1
   （假 peer 模拟 agentd 重启窗口：connect 成功但 AUTH 应答前断连）抓出
   `auth_locked` 把「ERR 被拒」和「AUTH 期间传输断」混为同一返回值 →
   断线被误报 GGERR_PERM 且永不重试。

   - 修复（wire.cpp）：`auth_locked` ERR 帧分支 return -2（被拒，配置问题
     不重试）；传输失败保持 -1；`ensure_connected_locked` 对 -1 走 continue
     （下一 attempt / 上层退避重试），-2 才 return -2。
   - 用例 d2 实证被拒不重试（仅 1 次连接 + 快速失败）；d3 实证 RST 中断
     发送 → 整帧未发完 → 安全重放成功。假 peer 测试框架在
     test_wire_io.cpp（真 socket 驱动真实收发路径，不 mock）。
   - **测试框架坑**：peer 线程的第二次 accept 在客户端不重试时永远阻塞，
     join 前必须 stop_listen（shutdown+close 打断阻塞中的 accept）。

51. **gg-ui 异步任务队列（P1-7，2026-11-08，GUI 行为未人工点验）**

   搜索同步调用卡 UI（全量 0.19s / 模糊首帧 0.32s）。解法是
   `tools/gg-ui/async_runner.h` 单 worker 队列 + 所有慢操作拆 exec/apply：

   - **并发语义（队列头注释 + test_async_runner.cpp 18 断言钉死）**：
     单 worker 串行 run；apply 只在 drain_completed() 的调用线程（UI）
     执行；不可取消；shutdown 丢弃未开始、跑完当前。
   - **铁律：worker 绝不碰 g_app / logf / ui_publish**。入参在 post 时
     快照进 job struct（worker 跑的时候用户可能在改输入框，直接读
     g_app 就是数据竞争）。g_app 的一切变更保持 UI 单线程语义。
   - 慢操作走 post_*（搜索家族/attach/connect/进程列表/模块列表/指针
     三件套）；快操作（单地址读写/冻结/undo/翻页）保持同步。
   - **AI 指令路径（ui_run_cmd）与启动 auto-* 用同步入口**（exec+apply
     顺序执行）：它们要立即读结果判断成败。别把 do_* 同步入口删了。
   - connect 的 swap 只在 UI 线程：worker new Session，apply 里
     delete 旧的再接管；busy 时连接按钮禁用，保证 swap 时没有 worker
     持有旧指针。
   - busy 时搜索家族按钮禁用 + 顶栏旋转字符；attach 允许排队。
   - 本机只验证了编译（-Wall 零警告）与队列单测；**GUI 实际交互
     （转圈/禁用/结果回填）未人工点验**，下次开 gg-ui 时顺带确认。

53. **工具参数映射的两道防线：静态护栏 + 假 agentd 端到端用例（2026-11-10）**

   PC 侧 62 个工具的参数映射此前没有任何测试：itest 直连 agentd 引擎，走不到
   session.cpp；test_session 只验证「非法枚举必须在 device_call 之前被拒」。
   于是「schema 声明了、agentd 也实现了、session.cpp 忘了塞进 params」这类
   缺陷完全静默 —— AI 传了没反应，还以为自己改到了行为。

   新增两层（都挂在 run_tests.bat，无设备也能跑）：

   - **check_tool_params.py（第 7 道结构护栏）**：静态解析 schema.cpp 的
     properties 顶层键，与 session.cpp 各 handler 在 args 上读的键
     （`a["k"]` / `.contains` / `.value` / `.at`，以及
     `for (const char* k : {"pc","fp",...})` 这种动态键循环）做双向差集：
     handler 读但 schema 没声明 = **ERR**（AI 永远传不出该参数）；
     schema 声明但 handler 不读 = **WARN**（传了无效）。白名单必须写理由
     （force 是故意拒绝守卫；gg_annotate_add 整包委派给 AnnotationStore::add）。
   - **test_tool_payloads.cpp**：本机 127.0.0.1 起假 agentd（端口 bind 0 自动选，
     避开真实 adb forward），走真 Session::call_tool → 真 AgentClient → 假对端，
     截下真正发出去的帧，断言键名与**规范化后的值**（addr 数字→"0x..."、
     枚举字符串→整数、aligned→0/1、缺省/可选参数不许凭空出现）。

   **抓到的三个真问题**（都已修）：
   1. `gg_search_range.aligned` 从未转发（schema 有、cmd_search.c 实现了、
      session.cpp 漏了一行）→ aligned=false 静默失效，查 packed 结构时得到
      一堆像模像样的非对齐巧合命中却看不出原因。
   2. `gg_hex_dump.save_to` 未声明：handler 与 gg_read_bytes 共用，落盘模式
      一直可用但 AI 不知道有这条路（大段 dump 只能撞 65536 的 hex 上限）。
   3. `gg_get_results.type` 早已废弃（agentd 按结果集自身类型读值），schema
      还挂着「已废弃/忽略」的声明，白占 AI 的注意力。

   **aligned 的 null 语义**：`parse_flag01` 把 null 当 0，若照搬就会发现
   "不传 = 1，传 null = 0" —— 默认值被悄悄翻成逐字节。所以判据是
   `contains("aligned") && !is_null()`，null 与缺省等价。

   **假对端框架坑（Windows）**：closesocket(listen) 不会打断阻塞中的 accept，
   closesocket(连接) 也不会让阻塞的 recv 自己醒 —— 两处都得主动掐，否则
   join() 永久挂住（现象是"断言全过但进程不退出"，很容易被误当成死循环）。
   accept 侧改用 50ms select 轮询 stop_ 标志。另：stdout 重定向时默认全缓冲，
   用例一旦挂住，已打印的 ok 行会整体丢失，看起来像"什么都没跑"，必须
   `setvbuf(stdout, nullptr, _IOLBF, 0)`。

   **非恒真验证**：把 `c.params["aligned"] = al;` 临时改成 `(void)al;`，
   用例立刻红（"FAIL: sr) aligned=false -> 0"）且退出码 1；恢复后全绿。

   **顺带干掉 test_session 的隐性网络等待**：f2 的 force=0 用例本来会去连本机
   34127（无设备时先跑 adb forward，再走 20s 指数退避），整轮单测凭空慢二十秒，
   而且结论取决于本机是否恰好跑着真 agentd —— 这不是"慢"，是**测试结果随环境
   漂移**。假对端抽成 `tests/unit/fake_agentd.h` 后两处共用：test_session 的
   该用例 20s → 42ms，并额外断言"请求真的发出去了 / addr 规范化了 / force 不在
   payload 里"（原断言只有"不是 BAD_REQUEST"，判别力太弱）。
   现在 `run_tests.bat` 全程不再有任何 adb / 真端口的痕迹。

57. **test_wire_io d3（写失败重放）的确定性条件（2026-11-12 修，此前 5 次 3 红）**

   d3 想验证 `AgentClient::call()` 的「整帧没发完 → 安全重连重放一次」，
   但原写法是"应答完 AUTH 立刻 RST"，实测大部分跑次根本没走到重放分支，
   却依然全绿 —— 断连落在 AUTH 应答读取阶段，走的是 d1 的重连路径。
   三个坑叠在一起，缺一个用例就退化成掷硬币或恒真：

   1. **必须先收到业务帧头再 RST**：`recv_exact(c1, &hdr, sizeof(hdr))`
      证明客户端已开始写业务请求，断连才落在"写"上而不是"读 AUTH"上。
   2. **请求体要大到写不完**：空 PING（20B 头 + `{}`）在 loopback 上经常
      在 RST 前整帧塞进发送缓冲（`wr==0`）—— 此时**不重放是正确行为**
      （对端可能已执行），断言按设计必红。用 512KB payload。
   3. **对端接收窗口要压小**：`SO_RCVBUF=8192`（listen socket 上设，accept 继承）。
      不压的话 Windows loopback 自动调优能把窗口涨到几百 KB，偶尔整帧交付成功。
   4. 非恒真验证：把 `call()` 里的重放分支临时关掉，d3 必须 3/3 红；
      开着重放 15/15 绿且每次都打印 `reconnect and replay once`。

   通用教训：**测"传输层失败"的用例，先问自己"失败一定发生吗"**。
   时序类断连必须构造出确定条件，否则绿灯本身就是噪声。
