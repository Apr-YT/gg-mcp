# 口算 PK 检测逻辑与自动化方案

> 目标：小猿AI（com.fenbi.android.leo v3.140.1）口算 PK 自动化答题
> 检测规避策略：零注入路线 + 行为拟人
> 关联文档：./小猿AI_API分析.md

---

## 1. PK 业务形态

| 维度 | 现状 |
|---|---|
| 入口 | H5（`leo-web-oral-pk`），`SimpleWebAppFireworkActivity` WebView 容器 |
| 主页 URL | `https://leo.fbcontent.cn/bh5/leo-web-oral-pk/...`（已加载，内存命中 9649 处 `oral-pk`） |
| 题目类型 | 20/100 以内数比大小、加减乘除、诗词填空、诗词 PK 等 |
| 提交方式 | H5 JS → 原生桥（`leo-bh5-native/audio` 等）→ `leo-game-pk` 网关 → 服务端 |
| 答题界面 | 题目 + 选项 + 倒计时（点击正确选项） |
| 实时性 | 与真人或机器人对手 PK 答题速度，毫秒级判定胜负 |

---

## 2. 检测面全图（防御方手段）

### 2.1 Native 层（`libmsaoaidsec.so` / `libmsaoaidauth.so` 静态实锤）

| 检测点 | 系统调用 | 检测目的 | 规避方式 |
|---|---|---|---|
| 反调试 | `ptrace`（主动 ptrace 自己） | 防止 IDA/GDB/Frida 调试 | **不主动调试**（纯外挂） ✅ |
| 模块遍历 | `dl_iterate_phdr` | 检测注入的 Xposed/Frida/Magisk so | **不注入**（纯 adb 外部） ✅ |
| 路径/重打包 | `readlink` `/proc/self/exe` | 查 APK 路径/签名 | **不改 APK** ✅ |
| 进程/线程遍历 | `opendir`/`readdir`/`fopen` | 遍历 `/proc` 查异常线程/工具 | **不创建新进程/线程** ✅ |
| 系统属性 | `__system_property_get` | 读 `ro.debuggable`/`ro.secure`/`ro.build.tags`/`ro.boot.vbmeta.device_state` | **真实设备**（非模拟器）✅ |
| 自保护 | `mprotect` + `__clear_cache` | .text 段运行时解密 | 与我们无关 |
| 进程控制 | `prctl`（PR_SET_DUMPABLE） | 禁止 coredump | 与我们无关 |
| 字符串加密 | `.rodata` 全乱码（`6f}xIk`/`Rx=+L` 等） | 关键检测串运行时解密 | 与我们无关 |
| 动态加载 | `dlopen` + `dlsym` | 加载检测插件/环境探测 | 与我们无关 |

**结论**：所有 native 检测针对**进程内注入/调试/重打包**。我们走**纯进程外**（OCR 屏幕 + adb 模拟点击），**零注入天然规避全部 native 检测**——与 CF/三角洲的 gg 直读策略同源。

### 2.2 行为埋点层（ThinkingData + 自研）

**实锤埋点格式**（内存抓取）：
```
AutoTrack-NETWORK: /leo-reward/android/task/hp::GET[200]
/local/LeoNetworkEventListener/callEnd {
  TotalFromRequestStartDuration, ConnectDuration, SecureConnectDuration,
  enqueueTimestamp, requestUrl, exerciseGrade, frogSource, VIPType, ...
}
/debug/leo/leoRequest { trace_id, enqueueTimestamp, ... }
/debug/leo/requestSucceed { ... }
```

**埋点上送字段（行为指纹）**：
- 每题答题耗时（start→answer 提交）
- 答题序列（题目 ID + 答案 + 正确性）
- 点击坐标（埋点可能上报 tap 事件）
- 网络时序（连接耗时、SSL 耗时）
- 设备姿态 `deviceOrientation=1`、`inBg=0`

**检测信号**：
- 答题耗时<800ms 且全对 = 疑似机器人
- 答题节奏等差（恒定间隔）= 机械化
- 毫秒级点击（无触摸滑动轨迹）= 非手指

### 2.3 服务端行为分析（推断）

| 风险 | 触发条件 | 后果 |
|---|---|---|
| 答题过快 | 每题<1s 全对连续多场 | 限流/降权/封号 |
| 胜率异常 | 连续 10+ 场 100% 胜率 | 行为审核 |
| 设备指纹变化 | 短时间内多设备/多 IP 同账号 | 风控 |
| 行为模式单一 | 每次答题路径/点击位置完全一致 | 行为分析识别 |

### 2.4 H5 前端（leo-web-oral-pk）防自动化

- 题目倒计时（每题 5-15s 限时）
- 题目 ID+答案 hash 防重放（服务端校验）
- JS 端可能做触摸事件检测（`touchstart`/`touchend` 时间差）

---

## 3. 自动化方案（三条路线对比）

### 方案 A：OCR 屏幕 + adb 模拟点击 ⭐ 推荐

**原理**：
```
截屏 → OCR 识别题目（数字+运算符）→ Python 计算 → adb input tap 点击答案按钮
```

**延迟分析**：
| 步骤 | 耗时 |
|---|---|
| `adb screencap` | ~50ms |
| OCR（数字识别） | ~150-300ms（轻量模型） |
| Python 计算 | <10ms |
| `adb input tap` | ~50ms |
| **总** | **~300-500ms** |

**检测影响**：
- ✅ 零注入：native 检测全规避
- ✅ 真实点击：`input tap` 触发真触摸事件（vs `sendevent` 合成事件，前者更真实）
- ⚠️ 行为埋点：每题都上报点击，需要拟人化

**风险与缓解**：
| 风险 | 缓解 |
|---|---|
| 答题过快被识别 | 注入随机延迟 800-1500ms |
| 100% 正确率异常 | 故意答错 8-15% |
| 节奏机械 | 延迟范围 + 高斯分布 |
| 单点 IP 异常 | 单账号/单设备，控制每日场次 |

**所需组件**：
- 截图：`adb shell screencap -p`
- OCR：PaddleOCR / Tesseract / 云 OCR（数字+运算符场景简单）
- 计算：Python `eval`（注意安全）或手动解析
- 点击：`adb shell input tap X Y`
- 循环：Python 主循环 + 状态机

### 方案 B：WebView 内存读题目明文（进阶）

**原理**：H5 JS 明文驻留内存，题目对象可被内存直接读出（跳过 OCR）。
- 需要定位 WebView 渲染进程（独立子进程）
- JS 题目结构（vue data / 局部变量）需逆向前端包
- 跨进程读内存（gg 需新 attach 到 webview 进程）

**对比**：
- ✅ 题目读取更准（无 OCR 误差）
- ❌ 复杂度高（跨进程 + JS 对象结构逆向）
- ❌ 收益低（数字 OCR 已经够准）
- 不推荐

### 方案 C：Frida JS 注入（不推荐）

**原理**：Frida hook WebView JS 函数，直接读题+答。
- ❌ 触发 `dl_iterate_phdr` 检测
- ❌ 需要 Frida 服务端 + 注入进程
- ❌ 违背"零注入"原则
- 不推荐

---

## 4. 方案 A 详细设计

### 4.1 技术栈

| 模块 | 选型 | 备注 |
|---|---|---|
| 截图 | `adb screencap` → PNG | 标准 adb 命令 |
| 数字 OCR | 裁剪 ROI → 模板匹配 / Tesseract / PaddleOCR | 口算题简单，可模板匹配优先 |
| 计算 | Python `re` + 自实现四则运算 | 不直接 `eval` 防止恶意字符串 |
| 点击 | `adb shell input tap X Y` | 真实触摸事件 |
| 主循环 | Python 异步 / 多线程 | 监听截屏变化 |
| 状态机 | 监听 UI 变化（截屏 diff） | 区分"题目出现"/"答题中"/"结算" |

### 4.2 UI 区域划分（基于实测截图）

```
┌─────────────────────────────┐
│  顶部状态栏（不处理）          │
│  [返回] [音乐] [巅峰对决]     │  ← y=0~250
├─────────────────────────────┤
│  PK 主页                     │  ← y=250~600
│  - 题目类型卡片              │
│  - [开始 PK] 按钮            │  ← y=830~870（点击入口）
├─────────────────────────────┤
│  答题界面（点开始后）          │
│  - 倒计时                    │  ← y=300
│  - 题目（大字）               │  ← y=500
│  - 答案选项（按钮区）          │  ← y=900~1500
└─────────────────────────────┘
```

**ROI 提取**（根据实测调整）：
- 题目区域：`y=400-600`（中等数字题目）
- 选项区域：`y=900-1500`（2-4 个按钮）

### 4.3 OCR 与答案映射

题目类型与答案特征：
- `a > b ?` → 两个数字比大小（20 以内数比大小）
- `a + b = ?` / `a - b = ?` / `a × b = ?` / `a ÷ b = ?` → 加减乘除
- 答案通常以按钮形式给出 2-4 个选项（可能包含正确和干扰项）

**OCR 策略**：
1. 截全屏
2. 裁剪题目区域
3. 数字识别（轻量 CNN 或模板匹配）
4. 解析运算符（位置特征：左数/右数/中间符号）
5. 计算结果
6. 扫描选项区域，匹配结果位置
7. tap 该位置

### 4.4 行为拟人化

```python
import random, time

def humanized_delay():
    """800-1500ms 高斯分布，模拟思考"""
    delay = random.gauss(1150, 200)
    return max(800, min(1500, delay))

def maybe_miss(correct_rate=0.9):
    """90% 正确率，故意答错"""
    return random.random() > correct_rate

def human_tap(x, y):
    """轻微随机偏移，模拟指尖不准"""
    dx = random.randint(-8, 8)
    dy = random.randint(-8, 8)
    subprocess.run(['adb', 'shell', 'input', 'tap', str(x+dx), str(y+dy)])
    time.sleep(humanized_delay() / 1000)
```

### 4.5 完整流程伪代码

```python
def pk_loop():
    while True:
        img = screencap()
        ui_state = detect_ui_state(img)  # 主页/答题/结算
        if ui_state == 'home':
            tap_start_pk()
            sleep(2)
        elif ui_state == 'answering':
            question = ocr_question(img)
            if question is None:
                sleep(0.3); continue
            if maybe_miss():
                answer = random_wrong_option(question)
            else:
                answer = compute_answer(question)
            tap_option(answer, img)
            sleep(humanized_delay() / 1000)
        elif ui_state == 'result':
            log_result()
            sleep(3)
        elif ui_state == 'rank':
            sleep(5)  # 等结算
```

---

## 5. 风险评估与对策

| 风险等级 | 风险 | 对策 |
|---|---|---|
| 🟢 低 | native 检测 | 零注入，全部规避 |
| 🟡 中 | 行为埋点上报 | 拟人化延迟+故意错+控制场次 |
| 🟡 中 | 服务端行为分析 | 单账号/单设备/单 IP/日场次限制 |
| 🟡 中 | H5 前端防自动化 | OCR 处理（不依赖 JS hook） |
| 🟠 高 | 答题耗时仍偏快 | 强制 800-1500ms 延迟 |
| 🟠 高 | 100% 胜率异常 | 故意答错 8-15% |
| 🟠 高 | 账号被举报/限流 | 仅自用研究，不公开宣传 |

---

## 6. 合规与边界

⚠️ **重要提示**：
1. 本方案仅用于授权安全研究、游戏辅助技术验证
2. 严格禁止用于：
   - 对战刷分、刷段位、刷奖品
   - 商业代练、批量账号
   - 破坏他人游戏体验
3. 服务条款约束：违反小猿AI 用户协议可能导致账号封禁
4. 数据保护：不采集/上传他人对战数据
5. 使用范围：仅本人账号测试

---

## 7. 待实现清单（按依赖顺序）

1. ☐ 安装 OCR 引擎（建议 PaddleOCR 轻量版或 ddddocr 数字模型）
2. ☐ 写截屏 + UI 状态检测（基于截屏差异/模板匹配）
3. ☐ 写 ROI 提取 + 数字 OCR
4. ☐ 写计算器（口算四则）
5. ☐ 写答案按钮定位（模板匹配或区域扫描）
6. ☐ 写状态机主循环
7. ☐ 写拟人化延迟 + 故意错逻辑
8. ☐ 写日志与统计
9. ☐ 用户实测调参（题目类型/选项布局/延迟范围）
