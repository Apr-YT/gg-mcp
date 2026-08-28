# Unity Camera ICall 逆向方法论（libunity.so）

> 2026-08-20 · CF 项目 · 纯静态分析（PC 侧，不碰设备/游戏）
> 目的：拿到 Camera 的 ViewMatrix（worldToCameraMatrix）native 读取偏移，为 CF ESP 提供数据源。

## 一、背景

CF（il2cpp + ACE）的 ESP 需要 ViewMatrix。但：

- C# 层 `UnityEngine.Camera` **只有事件字段**（onPreCull 等），矩阵在 native 对象
- `libil2cpp.so` 的 Camera 方法指针**全是懒绑定 thunk**（腾讯 API 加固：`adrp → ldr [x,#0x478~0x4a8] → 懒初始化 → br`，真实实现藏在 .bss 懒绑定槽，槽页无 PTE 驱动读不到）
- **`libunity.so`（Unity 引擎本体）没有加固**——名字字符串明文、ICall 注册表完整

结论：**矩阵从 libunity.so 静态侧挖，完全可行**。

## 二、样本与工具

| 项 | 值 |
|---|---|
| 样本 | `ai_workspace/temp/scratch/libunity.so`（19,501,520B ELF64 ARM64） |
| 设备来源 | `/data/app/~~…/com.tencent.tmgp.cf-…/lib/arm64/libunity.so` |
| 工具 | llvm-readelf（NDK r27d）、strings、Python struct + capstone（`envs/default` venv） |

## 三、ICall 注册表结构（核心）

### 1. 名字字符串明文

```
"UnityEngine.Camera::INTERNAL_get_worldToCameraMatrix"  @ .rodata vaddr 0xf7ef3b
```

libunity.so 的 .rodata **不加密**（腾讯只加固了 libil2cpp.so，引擎本体没动）。

### 2. 名字数组 ↔ 函数数组（1:1）

```
名字数组  @ .data.rel.ro 0x12814f8   ← 152 项，每项 8B 指针 → .rodata 方法名字符串
函数数组  @ 0x12819b8                ← 152 项，每项 8B 指针 → .text 函数（跳板）
```

**定位方法**（可复用）：
1. 从已知名字字符串 vaddr（0xf7ef3b）扫 .data.rel.ro，找**存该 vaddr 的 qword** → 找到注册结构/数组区
2. 往前扫连续 `.rodata` 指针找**名字数组起点**（0x12814f8）
3. 名字数组结束处（连续 .rodata 指针断点）= **函数数组起点**（0x12819b8，此处变 .text 指针）

### 3. 函数链（名字 → 跳板 → 真实实现）

| Camera 方法 | 函数数组值（跳板） | 真实实现 | 说明 |
|---|---|---|---|
| INTERNAL_get_worldToCameraMatrix | 0xa3bb14 | → 0x37ed3c → 0x382c38 | 矩阵缓存管理 → 核心计算 |
| INTERNAL_get_projectionMatrix | 0xa3bbc8 | → 0x383ce8 | FOV 类 getter 邻域 |
| INTERNAL_get_cullingMatrix | 0xa3d444 | ? | |

跳板模式（ARM64）：

```asm
adrp x?, #page            ; .bss/.data 懒绑定槽页
ldr  x?, [x?, #imm]       ; 读槽（真实函数指针）
cbnz x?, 已初始化         ; 非 0 → 直接跳
…懒初始化（解析+填槽）…
br   x?                   ; 跳真实实现
```

### 4. 矩阵偏移线索（0x37edc4 拷贝函数）

```
源矩阵 64B：  [x1+0x0  .. x1+0x30]   （4×16B，ldp q0-q3 搬运）
目标矩阵：    [x0+0x48 .. x0+0x78]
→ native Camera 矩阵 ≈ +0x48 或缓存区 +0x358（待运行时定稿）
```

## 四、可复用方法论（任何 Unity 游戏通用）

1. **名字字符串 → vaddr**：搜 `类名::INTERNAL_get_xxx` 明文（libunity 不加密）
2. **名字数组**：扫 .data.rel.ro 找存该 vaddr 的 qword → 往前找连续 .rodata 指针边界 = 起点
3. **函数数组**：名字数组结束 = 函数数组起点（.text 指针）
4. **真实实现**：函数数组值 = 跳板 → capstone 反汇编找 `br xN` 目标
5. **矩阵偏移**：真实实现里找 64B 矩阵搬运（`ldp q0-q3` / 连续 `ldr q`）→ 源/目标偏移
6. 注意：**函数数组索引可能有 1 项偏移**（名字[0]↔函数[1]），以跳板反汇编校准

## 五、与 CF 的关系（数据链路）

```
PlayerCamera.m_WorldCamera@0x10 → C# Camera 实例
  → m_CachedPtr@0x10 → native Camera 对象（libunity）
  → +0x48 或缓存区 +0x358 → worldToCameraMatrix / projectionMatrix（各 64B）
```

C# Camera 实例定位 = 对象锚定问题（UID 引用链延伸），native 对象 = 运行时特征搜索（见下）。

## 六、运行时验证（2026-08-20 对局实测，事实链实锤）

| 结论 | 证据（运行时反汇编） |
|---|---|
| **native Camera = C# Camera + 0x10** | `0x789b8300e4: ldr x0,[x0,#0x10]`（C#→native 转换 = m_CachedPtr） |
| **native Camera + 0x48 = 64B 矩阵** | `0x789b398dc4`：源 `[x1+0x0..0x30]` → 目标 `[x0+0x48..0x78]` |
| **native Camera + 0x358 = 矩阵缓存区** | w2cm 核心 `0x789b39cc38`：memcpy(x24, x19+0x358, ...)；+0x350 版本 / +0x398 索引 / +0x3a0 指针 / +0x3a8 数组 |
| **libunity GOT 可读** | `0x789c24xxxx` 槽值 = 真实函数地址（0x7c35 区）——与 libil2cpp/libunity .bss 懒绑定槽（PTE 防读，如 0x786b9563e8 / 0x789c454xxx）形成对比 |
| **CameraManager 结构** | +0x00 相机数组（元素步长 0x78）+0x10 数量 +0x18 标志；相机对象 +0x1e1 启用标志 +0x1f8 有效指针（0x789b2c3920 列表销毁 / 0x789b2c39b4 GetCameraByIndex） |

**卡点**：C# Camera 对象 / CameraManager 单例地址未锚定（libunity 数据区 PTE 防读 + 对象布局魔改）。投影矩阵特征（idx14=-1.0）搜索被堆对象结构（语音事件 Play_VO_* 等）干扰。

**替代**：CF 是 FPS，**角度法 W2S 不需要矩阵**（yaw/pitch + FOV=55 实测 + 坐标即可），见 `CF-ESP代码框架.md` 1.4。

## 七、待办（运行时，需对局）

- [ ] **native Camera 定位**：搜投影矩阵特征（m[3][2]=-1.0，列主序 idx14）
  - 字节模式（idx12-15）：`00 00 00 00 00 00 00 00 bf 80 00 00 00 00 00 00`
  - 命中 → 验证 64B 结构（fx>0/fy>0/其余 0）→ 找对象头 → 读 view matrix
- [ ] 与 `CF-ESP代码框架.md` 的 ViewMatrix TODO 对接
- [ ] 三角洲（同源 Unity）直接复用本方法论

## 七、关键资产索引

| 资产 | 位置 |
|---|---|
| libunity.so | `ai_workspace/temp/scratch/libunity.so` |
| 分析脚本 | `ai_workspace/scripts/extract_icall.py` / `disasm_icall.py` / `extract_cam.py` / `disasm_cam.py` / `extract_native_w2cm.py` / `disasm_native_w2cm.py` / `extract_native_real.py` |
| 函数字节 | `ai_workspace/temp/scratch/fns/`（icall_reg / w2cm_real / w2cm_final / w2cm_core / matrix_getter） |
| 相关文档 | `CF-ESP代码框架.md`（结构+W2S+遍历）、`FPS-ESP完整技术路线.md` |
