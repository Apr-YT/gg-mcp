# 手机悬浮窗方案

> 目标：把现有 APK 从独立 App 改成悬浮窗形式，同时增加实时日志页，显示电脑端 AI 通过 MCP 操作 agentd 的全过程。
> 手机端两个作用：① 手动 GG 工具（搜索/读写/断点） ② AI 操作过程显示器。
> AI 在电脑上跑，手机端不发指令给 AI。

---

## 一、架构

```
电脑端（操作方）                          手机端
┌─────────────────────┐                  ┌──────────────────────────────┐
│  AI 客户端           │                  │  悬浮窗 APK                   │
│  (豆包/Claude/...)   │                  │  ┌────────────────────────┐  │
│          │          │                  │  │ 标签栏                  │  │
│      MCP协议         │                  │  │搜索|内存|断点|标注|日志  │  │
│          ▼          │                  │  ├────────────────────────┤  │
│  gg-mcp-server.exe   │                  │  │ 内容区（切换）           │  │
│          │          │                  │  │                        │  │
│      wire协议        │                  │  │ 手动操作：               │  │
│          ▼          │                  │  │  搜索/筛选/结果          │  │
│  adb forward :19100  │                  │  │  内存hex/结构化视图      │  │
└──────────┬──────────┘                  │  │  断点设置/命中日志       │  │
           │                             │  │  标注列表                │  │
           │  adb forward                │  │                        │  │
           ▼                             │  │ AI操作显示：              │  │
      ┌──────────────────────────┐       │  │  实时滚动日志            │  │
      │   gg-agentd（手机端）      │       │  │  搜索结果同步            │  │
      │                          │       │  │  断点命中弹窗            │  │
      │  多连接：                 │       │  │  标注新增提示            │  │
      │  - 电脑 gg-mcp-server    │       │  └────────────────────────┘  │
      │  - 手机悬浮窗（只读+手动） │◀──────┤                              │
      │                          │ wire │  连接：127.0.0.1:19100        │
      │  事件帧广播给订阅端    │       │  （和 agentd 同机直连）      │
      └────────────┬─────────────┘       └──────────────────────────────┘
                   │
              内核驱动（内存读写/断点）
                   │
                   ▼
               游戏进程
```

**关键点**：
- agentd 支持多连接，电脑 gg-mcp-server 和手机悬浮窗同时连
- 电脑端发命令操作游戏，agentd 执行后把结果回给电脑，同时把事件帧广播给**订阅端**
- 手机悬浮窗连接后自动订阅（`GGCMD_SUBSCRIBE_EVENTS`），实时显示 AI 干了什么；
  PC 端默认不订阅（严格请求-响应零影响），`--print-events` 可开启订阅打印
- 手机悬浮窗也可以自己发命令（手动操作），和电脑端互不干扰

---

## 二、agentd 改造

### 2.1 多连接（已具备，无需 select 改造）

> ⚠️ 文档初稿假设 main.c 是单连接 accept 循环；**现状已是每连接一线程**
> （`accept` → `pthread_create(conn_thread)`），多连接天然支持，无需 select 重写。
> 本次实际改动只有：客户端注册表 + 事件广播 + 订阅命令。

多连接模型（现状）：

```
accept 循环 (main)                conn_thread(每连接一个)
  accept → pthread_create   →     read 帧 → dispatch() → 写响应
                                 dispatch 全程持 g_eng.mtx（串行，无并发冲突）
```

客户端注册表（事件广播用，`g_clients_mtx` 保护）：

```c
typedef struct gg_client {
    int fd;
    int subscribed;              /* 是否订阅事件流 */
    struct gg_client *next;
} gg_client_t;
static gg_client_t *g_clients = NULL;
static pthread_mutex_t g_clients_mtx = PTHREAD_MUTEX_INITIALIZER;
```

### 2.2 事件帧广播（订阅制）

`GGWF_EVENT` 标志位 `wire_protocol.h` 早已预留。**注意：改为订阅制** ——
PC 端 gg-mcp-server 是严格请求-响应（发一帧读一帧），直接给所有客户端广播事件会
打乱它的响应匹配。因此新增：

- `GGCMD_SUBSCRIBE_EVENTS` / `GGCMD_UNSUBSCRIBE_EVENTS`：客户端显式订阅后才收事件
- PC 端不订阅 → 行为完全不变（零回归）；手机悬浮窗连接后立即订阅
- PC 端可用 `--print-events` 订阅并打印事件帧（调试/监控），`call()` 读响应时
  跳过事件帧不干扰请求-响应

```c
static void broadcast_event(cJSON *ev) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, GG_J_EVT, ev);   /* {"ev":{...}} */
    char *s = cJSON_PrintUnformatted(root);
    pthread_mutex_lock(&g_clients_mtx);
    gg_client_t **pp = &g_clients;
    while (*pp) {
        gg_client_t *c = *pp;
        if (!c->subscribed) { pp = &c->next; continue; }
        if (send_frame(c->fd, 0, 0, GGWF_EVENT, s, strlen(s)) < 0) {
            /* best-effort：发送失败移除注册表项（fd 由连接线程自行关闭） */
            *pp = c->next;
            free(c);
            continue;
        }
        pp = &c->next;
    }
    pthread_mutex_unlock(&g_clients_mtx);
    free(s);
    cJSON_Delete(root);
}
```

事件帧：`cmd=0, seq=0, flags=GGWF_EVENT, payload={"ev":{...}}`

工程细节：
- 连接线程进入时 `clients_register(fd)`，退出时 `clients_unregister(fd)`
- 发送超时 SO_SNDTIMEO 从 60s 收紧到 5s（广播 best-effort，慢客户端最多拖 5s）
- `cmd_end` 事件在 `g_eng.mtx` 锁内发出（`g_last_resp` 为全局存档，跨连接读取需串行化）

### 2.3 事件推送点

| 位置 | 事件 type | 推送内容 | 实现 |
|---|---|---|---|
| dispatch 入口（所有命令统一） | `cmd_start` | 命令名 + 参数（>4KB 截断） | dispatch 层包裹 |
| dispatch 出口（所有命令统一） | `cmd_end` | 命令名 + 成功/失败 + 结果摘要 + 耗时 | send_ok/send_err 存档最近响应 |
| h_watch poll/inject（命中产出时） | `watch_hit` | pc + 模块 + 偏移 + 寄存器 + 预反汇编 | watch_hit_json() 复用 |
| search/refine/range/unknown_init/clear | `search_update` | 当前结果数 + 上次数 | 计数变化即推 |
| h_attach / h_detach | `session_change` | pid + 进程名 + 状态 | 成功/失败后推 |
| PC 端标注操作（gg_annotate_*） | `annotate` | 标注对象（tag/addr/type/value） | GGCMD_RELAY_EVENT 中继 |

> **标注事件**：标注库在 PC 端（`server/annotations.cpp`），PC 端每次
> `gg_annotate_add/remove` 成功后调 `GGCMD_RELAY_EVENT` 把标注对象转发给 agentd，
> agentd 再广播给订阅端（手机）。手机标注页即可实时看到 AI 的标注动作。

---

## 三、事件帧协议

统一格式：
```json
{"ev":{"type":"<类型>","ts":<毫秒时间戳>,"data":{...}}}
```

### cmd_start
```json
{"ev":{"type":"cmd_start","ts":1723812345000,"data":{"cmd":"gg_search","params":{"value":"30","type":"dword"}}}}
```

### cmd_end
```json
{"ev":{"type":"cmd_end","ts":1723812345200,"data":{"cmd":"gg_search","success":true,"result":{"count":25431},"duration_ms":200}}}
```
失败：`{"success":false,"error":"no session","code":3}`

### watch_hit
```json
{"ev":{"type":"watch_hit","ts":...,"data":{
  "tid":12345,
  "pc":"0x7f1234567890",
  "hit_addr":"0x7fabcdef0000",
  "module":"libil2cpp.so",
  "module_base":"0x7f0000000000",
  "module_offset":"0x12345678",
  "regs":{"x0":"0x1e","x1":"0x7f...","x2":"0x1e","fp":"...","sp":"...","lr":"..."},
  "disasm":[{"addr":"0x7f...","mnemonic":"str","op_str":"w0, [x1]","comment":"store"}],
  "stack":["0x7f...","0x7f..."]
}}}
```

### search_update
```json
{"ev":{"type":"search_update","ts":...,"data":{"count":4,"prev_count":25431}}}
```

### session_change
```json
{"ev":{"type":"session_change","ts":...,"data":{"state":"attached","pid":12345,"name":"com.tencent.tmgp.sgame"}}}
```

### annotate（PC 端标注经 agentd 中继）
```json
{"ev":{"type":"annotate","ts":...,"data":{"op":"add","ann":{"tag":"ammo_current","addr":"0x7f...","type":"dword","value":"30","note":"当前子弹数"}}}}
{"ev":{"type":"annotate","ts":...,"data":{"op":"remove","ann":{"tag":"ammo_current"}}}}
```

---

## 四、悬浮窗 APK 改造

现有 APK（`apk/` 目录）已有：
- `WireClient`（wire 协议客户端，本次重写为异步读线程 + 事件回调 + 自动重连）
- 状态页 / 进程页 / 搜索页 / 内存hex页（搬进悬浮窗，逻辑进 `OverlayUi.java`）
- GG 风格深色主题

**实现说明**：工程为纯命令行 aapt2/javac/d8 构建（无 Gradle/无 AndroidX），
因此标签页用 **View 面板切换**（非 Fragment）、日志列表用 **ListView**（非 RecyclerView）。
MainActivity 降级为"权限申请 → 启动服务"引导页，全部 UI 在悬浮窗内。

### 4.1 权限（已实现）

`AndroidManifest.xml`：
```xml
<uses-permission android:name="android.permission.SYSTEM_ALERT_WINDOW"/>
<uses-permission android:name="android.permission.FOREGROUND_SERVICE"/>
<uses-permission android:name="android.permission.FOREGROUND_SERVICE_SPECIAL_USE"/>
```
- `<service android:name=".FloatWindowService" android:foregroundServiceType="specialUse">`
  + `PROPERTY_SPECIAL_USE_FGS_SUBTYPE`（targetSdk 34 必需）
- 动态申请：`Settings.ACTION_MANAGE_OVERLAY_PERMISSION`（MainActivity 引导）
- MIUI：`dumpsys deviceidle whitelist +包名` + `am set-standby-bucket 包名 active`（防后台冻结，需手动执行）

### 4.2 前台服务（已实现，`FloatWindowService.java`）

- onCreate：NotificationChannel + `startForeground`（通知：GG-MCP 悬浮监控 · 连接状态）
  + 悬浮球 + `OverlayUi` 构建悬浮窗 + WireClient 事件监听/自动重连
- 悬浮球：可拖动（移动 x/y），点击展开/收起悬浮窗（addView/removeView）
- 悬浮窗：标题栏可拖动整个窗口，右下角「◢」可调尺寸（260x360dp 起，上限全屏）
- 断点命中（watch_hit 事件）→ 顶部弹小窗提示 3 秒（无论当前在哪个标签）
- onDestroy：停重连、断开、移除所有窗口

### 4.3 悬浮球（已实现）

- `TextView` "GG"，`TYPE_APPLICATION_OVERLAY` + `FLAG_NOT_FOCUSABLE`
- 触摸：按下记录位置 → 移动更新 `x/y` → 抬起位移 <8dp 判定为点击
- 点击：`togglePanel()`（addView/removeView 切换悬浮窗显隐）

### 4.4 悬浮窗（已实现，`float_window.xml`）

- 尺寸：宽 85% 屏幕宽，高 65% 屏幕高（可拖动右下角调整）
- 背景：`float_bg.xml` 圆角 12dp 半透明深色（#E61A1A2E）
- 顶部标题栏：连接状态指示灯（绿=已连接/红=断开）+ 进程名 + 收起 + 关闭，可拖动
- 中部内容区：`FrameLayout` 切换 6 个 View 面板
- 底部状态条：结果数 / 断点命中数 / 标注数

### 4.5 标签栏（6个标签，已实现）

| 标签 | 内容 | 来源 |
|---|---|---|
| 搜索 | 数值输入 + 类型选择 + 搜索/refine + 结果列表 + 冻结勾选 | 原搜索页搬入 OverlayUi |
| 内存 | 地址输入 + hex 视图 + 上一页/下一页 + 结构化视图（int32/float32/pointer，gg_view_memory） | 原内存页搬入 + 结构化新增 |
| 断点 | 地址 + 类型(读/写/读写/执行) + 设置/清除/轮询 + 命中列表 | 新增，调 gg_watch；命中经 watch_hit 事件自动进列表 |
| 标注 | 字段列表（tag/地址/类型/值）+ 本机手动添加 + PC 中继同步 | 新增；agentd 无 annotate 命令，PC 端经 RELAY_EVENT 同步，本机可手动记 |
| 日志 | 实时滚动，显示所有操作（AI 的 + 手动的）+ 过滤 + 导出 | 新增，收事件帧 |
| 设置 | 引擎状态 + 启动引擎 + 进程列表（点击附加）+ 分离 | 状态页 + 进程页合并 |

### 4.6 日志页（已实现，`panel_log.xml` + LogAdapter）

`ListView` 显示事件列表（无 AndroidX → 不用 RecyclerView）：

```
┌───────────────────────────────────────┐
│ 20:31:02  gg_search                    │  灰色小字
│   {value:"30", type:"dword"}          │  等宽浅灰
│   → 25431 条 (200ms)                  │  白色
├───────────────────────────────────────┤
│ 20:31:45  ⚡ 断点命中!                 │  红色高亮
│   libil2cpp.so+0x12345                │
│   x0=0x1e x1=0x7f... x2=0x1e         │
├───────────────────────────────────────┤
│ 20:32:01  ✓ 标注: ammo_current        │  绿色
│   0x7f... dword 当前子弹数             │
└───────────────────────────────────────┘
```

着色规则（LogAdapter.getView）：
- `cmd_start`：标题灰色，参数浅灰等宽
- `cmd_end` 成功：标题绿色，结果白色
- `cmd_end` 失败：整行红色
- `watch_hit`：整行深红底高亮
- `search_update`/`session_change`：归入命令类
- `annotate`：绿色

功能：
- 自动滚动到底部；用户上滑查看历史时暂停（`getLastVisiblePosition() >= n-2` 才滚）
- 过滤：全部/命令/断点/标注 四个 CheckBox
- 导出：全部日志写到 `getExternalFilesDir/ggmcp_log_<ts>.txt` 并 toast 路径
- 上限 4000 条（超出丢最旧）

### 4.7 WireClient 改造（已实现，异步读线程版）

单连接 + 后台读线程：

```
call()（任意线程）                  readLoop()（读线程）
  注册 Pending(seq)                 读帧
  ──写请求帧──────▶                  ├─ GGWF_EVENT → EventListener（日志页/命中弹窗）
  wait(120s)                         └─ 响应帧 → 按 seq 匹配 Pending 唤醒
  ←──返回/抛 WireException
```

- 事件回调：`setEventListener(ev -> ...)`，主线程转交 OverlayUi
- 连接状态回调：`setConnStateListener(ok, err)` → 悬浮窗指示灯
- 自动重连：`startAutoReconnect(PORT)` 指数退避 1s→2s→4s→…→30s 封顶；
  重连成功后**自动重新订阅事件流**（agentd 订阅按连接计，新连接需重发 SUBSCRIBE）
- 连接地址：`127.0.0.1:19100`（和 agentd 同机，不需要 adb forward）

---

## 五、开发顺序（状态：全部已完成 ✅）

### 第一阶段：跑通 ✅

1. **agentd 多连接**：现状已是每连接一线程（无需 select 改造）
2. **agentd 事件广播**：客户端注册表 + `broadcast_event()`（订阅制）+ dispatch 层 cmd_start/cmd_end（覆盖全部命令）
3. **悬浮窗框架**：权限申请 + 前台服务 + 悬浮球 + 悬浮窗容器
4. **日志页**：ListView + 事件解析 + 着色 + 自动滚动
5. **WireClient 事件接收**：连 127.0.0.1:19100 + 异步读线程 + 自动重连 + 自动订阅

**验收**：电脑上 AI 搜内存，手机悬浮窗日志页实时显示 `gg_search → 25431条 → gg_search_refine → 4条`。

### 第二阶段：手动操作搬入悬浮窗 ✅

6. **搜索页**：原搜索 UI 搬入 OverlayUi（View 面板，非 Fragment）
7. **内存页**：原 hex 页 + gg_view_memory 结构化视图（int32/float32/pointer）
8. **进程/设置页**：状态页 + 进程列表合并到设置标签
9. **断点页**：gg_watch set/clear/poll + 命中列表 + 寄存器显示（watch_hit 事件自动进列表）
10. **标注页**：本机手动标注 + PC 端 gg_annotate_* 经 RELAY_EVENT 中继实时同步

### 第三阶段：完善 ✅

11. ✅ 断点命中时悬浮窗顶部弹小窗提示 3 秒（任何标签可见）
12. ✅ 日志过滤（全部/命令/断点/标注 四个开关）
13. ✅ 日志导出（getExternalFilesDir 保存文件 + toast 路径）
14. ✅ 悬浮窗尺寸可调（右下角「◢」拖动，260x360dp 起）
15. ✅ 电脑端 `gg-mcp-server --print-events` 订阅并打印事件帧（call() 读响应时跳过事件帧，不干扰请求-响应）

---

## 六、注意事项

1. **agentd 串行处理**：所有客户端请求串行执行（`g_eng.mtx`），事件在请求处理完成（锁内）后广播。
2. **广播 best-effort**：发送失败的客户端从注册表移除（不 close，连接线程自行收尾），不影响请求方拿结果。
3. **手机和电脑同时操作**：两边都能发命令，agentd 串行执行，不会冲突。但建议不要同时对同一个地址操作。
4. **MIUI 后台冻结**：必须前台服务 + 电池白名单，否则悬浮窗服务被冻结导致连接断开。
5. **电脑端连 agentd 走 adb forward**：`adb forward tcp:19100 tcp:19100`，和现在一样。
6. **手机端连 agentd 走 127.0.0.1**：同机直连，不需要 adb forward，延迟低。
7. **事件订阅制**：PC 端默认不订阅 → 严格请求-响应零影响；手机连接后发 `GGCMD_SUBSCRIBE_EVENTS` 订阅；`--print-events` 可让 PC 端也订阅（调试）。
8. **cmd_end 事件在锁内发出**：`g_last_resp` 是全局存档，跨连接读取需在 `g_eng.mtx` 内串行化。
9. **SO_SNDTIMEO 收紧到 5s**：广播 best-effort，慢客户端最多拖 5s。

---

## 七、文件改动清单（实际）

| 文件 | 改动 | 说明 |
|---|---|---|
| `shared/wire_protocol.h` | 小改 | 新增 GGCMD_SUBSCRIBE_EVENTS / UNSUBSCRIBE / RELAY_EVENT 命令码 |
| `agentd/main.c` | 中改 | 客户端注册表 + broadcast_event + cmd_start/cmd_end（dispatch 层）+ watch_hit/search_update/session_change 推送 + 订阅/中继 handler |
| `agentd/watch.c` | 不改 | 命中数据由 h_watch poll/inject 经 watch_hit_json() 统一产出并广播 |
| `server/device/wire.h` | 小改 | AgentClient::Config 加 print_events |
| `server/device/wire.cpp` | 小改 | 连接后按 print_events 订阅；call() 读响应时跳过并打印事件帧 |
| `server/session.h` | 小改 | ServerConfig 加 print_events；声明 relay_annotate |
| `server/session.cpp` | 小改 | gg_annotate_add/remove 成功后 relay_annotate 中继到 agentd |
| `server/main.cpp` | 小改 | --print-events 参数 + config 字段 |
| `apk/AndroidManifest.xml` | 小改 | 悬浮窗/前台服务权限 + specialUse service |
| `apk/java/com/ggmcp/app/FloatWindowService.java` | 新增 | 前台服务 + 悬浮球 + 悬浮窗管理 + 命中弹窗 + 自动重连 |
| `apk/java/com/ggmcp/app/OverlayUi.java` | 新增 | 6 标签面板（搜索/内存/断点/标注/日志/设置）+ 事件渲染 |
| `apk/java/com/ggmcp/app/WireClient.java` | 重写 | 异步读线程 + 事件回调 + Pending 匹配 + 自动重连/订阅 |
| `apk/java/com/ggmcp/app/MainActivity.java` | 重写 | 权限申请引导 + 启动 FloatWindowService |
| `apk/res/layout/float_window.xml` 等 | 新增/改造 | 悬浮窗容器/球/日志/断点/标注/行布局；activity_main 改引导页 |
| `apk/res/drawable/float_bg.xml`, `ball_bg.xml` | 新增 | 圆角背景 + 悬浮球 |
| `docs/FLOAT_WINDOW_MONITOR.md` | 更新 | 与实现对齐 |
