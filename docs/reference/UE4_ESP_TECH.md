# UE4 FPS ESP 最小技术说明文档

> 目标：最小化 ESP 实现，只讲核心原理、关键结构体、内存偏移、渲染链路、最小可行代码思路，去掉冗余内容，用于 FPS 游戏角色显示。
>
> 适用：UE4 4.2x‑4.26 主流 FPS 项目，安卓端独立 ELF 外部绘制，内核驱动读内存 + ANativeWindow + ImGui（Vulkan 后端），仅用于技术研究。

## 目录

- [1. 核心概念](#1-核心概念)
- [2. 关键全局与结构体](#2-关键全局与结构体圈子常用写法)
- [3. W2S 世界转屏幕核心算法](#3-w2s-世界转屏幕核心算法最小公式)
- [4. 数据获取与更新流程](#4-数据获取与更新流程每帧遍历)
- [5. 渲染方案](#5-渲染方案纯-elf-外部绘制)
- [6. ESP 绘制元素最小规格](#6-esp-绘制元素最小规格)
- [7. 偏移获取方法](#7-偏移获取方法)
- [8. 常见坑点](#8-常见坑点最小排错)
- [9. 最小项目文件结构](#9-最小项目文件结构)
- [10. 安卓平台：纯 ELF 外部绘制方案](#10-安卓平台纯-elf-外部绘制方案)
- [11. 安全与反检测（进阶）](#11-安全与反检测进阶)

***

## 1. 核心概念

ESP（Extra‑Sensory Perception）：读取 UE4 游戏进程内存，解析 Actor 实体数据，把敌人 / 队友位置、血量、名字绘制到屏幕上。

**两大模块：**

| 模块           | 职责                    |
| ------------ | --------------------- |
| 内存读取模块       | 遍历 UE4 对象，拿到角色世界坐标、状态 |
| 世界转屏幕（W2S）模块 | 3D 世界坐标 → 2D 屏幕坐标     |
| 渲染模块         | 矩形框、血条、文字绘制           |

**UE4 关键链路：**

```
GWorld → ULevel → AActor → APawn → ACharacter
```

## 2. 关键全局与结构体（圈子常用写法）

### 2.1 跨进程读内存：内核驱动 ioctl（唯一方式）

采集端与绘制端为**同一个独立 ELF 可执行进程**（不注入游戏），需 root，跨进程读取游戏内存。标准写法为**内核驱动 ioctl**：用户态直接与内核驱动通信，绕过 libc 与游戏进程内一切 hook，抗用户态 hook / 反作弊检测。

```cpp
// 内核驱动通信协议（各驱动大同小异）
enum { OP_INIT_KEY=0x800, OP_READ_MEM=0x801, OP_WRITE_MEM=0x802, OP_MODULE_BASE=0x803 };

typedef struct {                       // COPY_MEMORY
    pid_t     pid;
    uintptr_t addr;
    void*     buffer;
    size_t    size;
} COPY_MEMORY;

typedef struct { pid_t pid; char name[64]; uintptr_t base; } MODULE_BASE;

int OpenDriver();                      // 见下方节点发现

template <typename T>
bool Read(int fd, pid_t pid, uintptr_t addr, T& out) {
    COPY_MEMORY cm{ pid, addr, &out, sizeof(T) };
    return ioctl(fd, OP_READ_MEM, &cm) == sizeof(T);
}
```

驱动节点自动发现（实战写法）：扫描 `/proc/*/exe` 找 `(deleted)` 的驱动进程 → 解析设备号 `mknod` 重建 `/dev` 节点 → 连接后立即 `unlink` 删除（**用完即删，防静态扫描**）。模块基址用 `OP_MODULE_BASE` 直接向驱动要 `libUE4.so` 地址，无需解析 maps。

> 不采用用户态 `process_vm_readv` / `/proc/pid/mem`：实现虽简单，但暴露在用户态，易被游戏内 hook 与反作弊检测。

### 2.2 全局指针与偏移表

`GWorld` 为全局变量，静态地址 / Pattern 扫描获取；成员偏移集中到 `Offsets` 命名空间统一管理，按版本扫描 / SDK 生成：

```cpp
namespace Offsets {
    // 以下为示例值，实际需按目标版本扫描获取
    constexpr uintptr_t GWorld          = 0x4E3C2A0; // UWorld*
    constexpr uintptr_t PersistentLevel = 0x30;      // UWorld → ULevel
    constexpr uintptr_t Actors          = 0x98;      // ULevel → TArray Data
    constexpr uintptr_t ActorCount      = 0xA0;      // TArray Count
    constexpr uintptr_t RootLocation    = 0x128;     // 世界坐标（根组件）
    constexpr uintptr_t Health          = 0x1C0;     // ACharacter 血量
    constexpr uintptr_t MaxHealth       = 0x1C4;
    constexpr uintptr_t ViewProjection  = 0x2A0;     // PlayerCameraManager 相机矩阵
}
```

**真实游戏偏移链示例（暗区突围，libUE4.so 静态偏移）：**

```cpp
// GWorld / 世界链
Uworld  = Read<uintptr_t>(libUE4 + 0xf429248);           // GWorld
ULevel  = Read<uintptr_t>(Uworld + 0x30);                // PersistentLevel
数据地址 = Read<uintptr_t>(ULevel + 0x98);                // TArray Data
数量    = Read<int32_t>(ULevel + 0xA0);                   // TArray Count

// 视图投影矩阵（W2S 用，16 个 float）
matrix = Read<uintptr_t>(libUE4 + 0xf3dac88);
matrix = Read<uintptr_t>(matrix + 0x20) + 0x2A0;         // 一次性读 16×4 字节

// 角色字段（每个 Actor）
血量 = Read<float>(actor + 0xD18);  护盾 = Read<float>(actor + 0xD1C);
名字 = getUTF8(Read<uintptr_t>(Read<uintptr_t>(actor + 0x330) + 0x3E8));
```

**骨骼读取（USkeletalMeshComponent，实战场合通用）：**

```cpp
uintptr_t mesh  = Read<uintptr_t>(actor + 0x370);        // ACharacter→Mesh
int32_t   count = Read<int32_t>(mesh + 0x828 + 0x8);     // BoneCount
uintptr_t bones = Read<uintptr_t>(mesh + 0x828) + 0x30;  // 骨骼数组
// 每根骨骼一个 FTransform（Rotation 四元数 + Translation + Scale3D），
// 步长固定（如 48 字节），读取后四元数转矩阵 × ComponentToWorld 合成世界坐标
```

### 2.3 关键结构体（类型参考）

读取时不实例化完整对象，只按偏移 `Read` 单字段；结构体仅作类型对照：

```cpp
struct UWorld        { void* PersistentLevel; };        // ULevel*
struct ULevel        { TArray<void*> Actors; };         // TArray<AActor*>
struct AActor        { FVector RootLocation; void* RootComponent; };
struct ACharacter : AActor { float Health, MaxHealth; void* PlayerState; };
struct FVector       { float X, Y, Z; };
struct FMatrix       { float M[4][4]; };

// UE4 原生数组
template<typename T>
struct TArray { T* Data; int32 Count; int32 Max; };
```

### 2.4 相机矩阵（W2S 来源）

- 标准路径：`APlayerCameraManager` 提供 `ViewProjectionMatrix`（视图投影矩阵），即 `GWorld → GetFirstLocalPlayerController() → PlayerCameraManager`。
- 实战更常用**全局矩阵指针链**（如 2.2 节示例 `libUE4+0xf3dac88 → +0x20 → +0x2A0`）：省掉 4~5 次指针跳转，一帧只读 16 个 float。
- 第 4 / 10 节代码中的 `g_Camera` 即该全局矩阵基址，W2S 前读取后填 `Offsets::ViewProjection`。

## 3. W2S 世界转屏幕核心算法（最小公式）

- 输入：角色 `FVector WorldPos`、`ViewProjectionMatrix`、屏幕宽高
- 输出：屏幕 2D 坐标 `x, y`；返回 `false` 代表在相机视锥外，不需要绘制

```cpp
bool WorldToScreen(FVector WorldPos, FMatrix ViewProj, int ScreenW, int ScreenH, ImVec2& Out)
{
    // 矩阵相乘，得到裁剪空间
    float x = ViewProj.M[0][0]*WorldPos.X + ViewProj.M[0][1]*WorldPos.Y + ViewProj.M[0][2]*WorldPos.Z + ViewProj.M[0][3];
    float y = ViewProj.M[1][0]*WorldPos.X + ViewProj.M[1][1]*WorldPos.Y + ViewProj.M[1][2]*WorldPos.Z + ViewProj.M[1][3];
    float w = ViewProj.M[3][0]*WorldPos.X + ViewProj.M[3][1]*WorldPos.Y + ViewProj.M[3][2]*WorldPos.Z + ViewProj.M[3][3];

    if(w < 0.1f) return false; // 在相机后面，丢弃

    float ndcX = x / w;
    float ndcY = y / w;

    Out.x = ( ScreenW * 0.5f ) * (ndcX + 1.0f);
    Out.y = ( ScreenH * 0.5f ) * (1.0f - ndcY);
    return true;
}
```

> 重点：`w` 值判断，`w` 过小物体在镜头后方，直接跳过绘制，避免画面错乱。

## 4. 数据获取与更新流程（每帧遍历）

核心思路：**渲染循环内每帧同步遍历 Actor → W2S → 直接绘制**，无缓存、无中间结构。内核驱动读内存极快（单次 ioctl 数十微秒），每帧遍历是主流写法，实时性最好。

### 4.1 渲染主循环（每帧遍历）

```cpp
// 每帧：读世界链 + 相机矩阵 → 遍历 Actor → 直接绘制
int drv = OpenDriver();                     // 驱动节点，用完即删
while (g_Running) {
    uintptr_t world, level, data;
    int32_t   count;
    if (!Read(drv, pid, g_Base + Offsets::GWorld, world))        continue;
    if (!Read(drv, pid, world + Offsets::PersistentLevel, level)) continue;
    if (!Read(drv, pid, level + Offsets::Actors, data))          continue;
    if (!Read(drv, pid, level + Offsets::ActorCount, count))     continue;
    if (count <= 0 || count > 1024)                              continue;

    FMatrix viewProj;                       // 相机矩阵每帧重读，避免 W2S 错位
    Read(drv, pid, g_Camera + Offsets::ViewProjection, viewProj);

    for (int32_t i = 0; i < count; i++) {
        uintptr_t actor;
        if (!Read(drv, pid, data + i * sizeof(uintptr_t), actor) || !actor) continue;
        // 类型判断（虚表）、过滤本地玩家与死亡角色（省略）

        FVector pos; float hp, maxHp;
        Read(drv, pid, actor + Offsets::RootLocation, pos);
        Read(drv, pid, actor + Offsets::Health, hp);
        Read(drv, pid, actor + Offsets::MaxHealth, maxHp);

        ImVec2 scr;
        if (!WorldToScreen(pos, viewProj, W, H, scr)) continue;  // 视锥外跳过
        DrawBox(scr);                        // 方框
        DrawHealth(scr, hp, maxHp);          // 血条
        DrawName(scr);                       // 名字
    }
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd); // 提交绘制命令
    vkQueuePresentKHR(queue, &present);      // 提交给 SurfaceFlinger 上屏
}
```

**流程要点：**

1. 每帧读取 `GWorld → PersistentLevel → Actors/Count`（按偏移表逐级 Read）
2. 每帧重读相机 `ViewProjection` 矩阵（W2S 用，避免矩阵过期错位）
3. 循环遍历 `Data[0 ... Count‑1]`，校验指针与类型，过滤本地玩家、死亡角色
4. 逐个 W2S 投影后**直接绘制**（方框 / 血条 / 名字），无需缓存与线程同步

**过滤要点：**

- 跳过本地玩家（LocalPlayerController 的 Pawn）
- 跳过死亡角色（Health ≤ 0）
- W2S 返回 `false` 直接跳过绘制

> 不采用快照式（定时批量读 + 共享内存缓存）：多一层中间副本、有更新滞后，仅在读内存慢（如用户态跨进程）时才值得；驱动 ioctl 场景统一用每帧遍历，实时性最好。

## 5. 渲染方案：纯 ELF + SurfaceFlinger Overlay

唯一主线：**独立 ELF 进程经 SurfaceFlinger 创建覆盖层（layer）直绘**，ImGui 渲染，与游戏画面叠层显示。

- 绘制端为**纯 ELF 可执行文件**（root 运行，不注入游戏），通过 Binder 连系统 SurfaceFlinger 创建置顶 layer
- 渲染链路：**ANativeWindow + ImGui + Vulkan**，Vulkan（ImGui_ImplVulkan）在**自有** surface / swapchain 上绘制，不碰游戏渲染链
- 每帧流程：驱动 ioctl 读内存（第 4 节）→ W2S（第 3 节）→ ImGui 绘制 → `vkQueuePresentKHR` 提交上屏
- 图层渲染实现见[第 10 章](#10-安卓平台纯-elf-外部绘制方案)

**不采用注入式方案的原因：**
- UCanvas Hook（内部 UE4 绘制）：需注入游戏进程、处理 UE4 渲染上下文，反作弊检测风险高
- Vulkan Hook（Hook 游戏进程的 `vkQueuePresentKHR`）：需处理游戏 swapchain / ABI 差异，游戏崩溃即崩绘制，易被反作弊检测

**优点：** 不 Hook 游戏渲染，游戏崩溃不影响绘制；无 App、无注入，数据读取与渲染同进程解耦。
**缺点：** 需要 root；图层由 SurfaceFlinger 合成，帧率受其调度影响。

## 6. ESP 绘制元素最小规格

- 包围盒：`ImGui::GetBackgroundDrawList()->AddRect(top_left, bottom_right, IM_COL32(255,0,0,255))`
- 血条：左侧垂直条；按 `Health / MaxHealth` 计算高度；绿色代表满血，红色残血。
- 角色名：角色上方文字标签。

> 注意：UE4 拿到的是 Actor 名字，很多游戏会加密，不一定直接可读。

## 7. 偏移获取方法

- IDA / Ghidra Pattern 扫描 GWorld 全局签名
- UE4 SDK Generator 生成 SDK 头文件，拿到结构体偏移
- ReClass 实时调试内存，验证成员偏移

> 注意：不同游戏版本偏移完全不一样，不能直接复制别人偏移。

## 8. 常见坑点（最小排错）

| 现象       | 原因                                                   |
| -------- | ---------------------------------------------------- |
| W2S 坐标乱飞 | `ViewProjectionMatrix` 拿错；矩阵行 / 列顺序 UE4 是行主序，不要当成列主序 |
| 角色在身后也绘制 | 忘记 `w < 0.1` 判断                                      |
| 遍历崩溃     | TArray 的 `Count` 越界，`GWorld` 空指针，Actor 空指针           |
| 包围盒高度不对  | 只用 `RootLocation`（脚底），没有获取角色头顶坐标                     |

> 最小实现只用 `RootLocation`，简易 ESP；精准包围盒需要读取 `CapsuleComponent` 半高。

## 9. 最小项目文件结构

目录组织采用 `jni/`（ndk-build）布局：`include/` 放头文件、`src/` 放实现，构建用 `Android.mk + Application.mk` 产出**独立 ELF**：

```
esp/                                  // jni/（ndk-build 项目根）
 ├─ Android.mk                        // include $(BUILD_EXECUTABLE) 产出 ELF（C++17）
 ├─ Application.mk                    // APP_STL=c++_static  APP_ABI=arm64-v8a  APP_PLATFORM=android-21
 ├─ include/                          // 头文件集中区
 │   ├─ main.h                        // 全局声明、常量、类名
 │   ├─ Android_draw/                 // 绘制接口头（draw.h）
 │   ├─ Android_touch/                // 自瞄触控接口头（touch.h，uinput 虚拟触摸）
 │   ├─ Android_shm/                  // memfd 匿名内存加载封装（shm_open_anon.h）
 │   ├─ native_surface/
 │   │   └─ native_surface.h          // CreateNativeWindow/DestroyNativeWindow/GetScreenSize
 │   └─ res/                          // Encrypt.h（字符串加密）
 ├─ src/                              // 实现文件集中区
 │   ├─ main.cpp                      // 入口：OpenDriver → 建 layer → 每帧主循环
 │   ├─ Android_draw/
 │   │   ├─ draw.cpp                  // 偏移链 + 每帧遍历 Actor + 绘制
 │   │   └─ 内核读写/
 │   │       ├─ 内核读写.h            // 驱动 ioctl 封装（协议 + 节点发现/清理）
 │   │       ├─ 函数.h                // Read<T> 模板、进程/基址获取
 │   │       └─ 过缺页.h              // mincore 预检防缺页
 │   ├─ Android_shm/                  // shm_open_anon.cpp（memfd 实现）
 │   ├─ Android_touch/                // touch.cpp（uinput 自瞄触控实现）
 │   └─ native_surface/               // native_surface.cpp（自研最小符号集 + 加密，见 10.3 ①）
 └─ third_party/                      // 第三方库（clone/下载，不提交）
     ├─ imgui/                        // ImGui 核心 imgui.cpp/imgui_draw.cpp/imgui_tables.cpp/imgui_widgets.cpp
     │   └─ backends/                 // imgui_impl_android.cpp / imgui_impl_vulkan.cpp
     └─ cjson/                        // cJSON.c / cJSON.h（配置文件解析）
```

**拆分建议（与目录配套）：**

- 按职责拆实现：`内核读写/`（驱动 ioctl 封装）、`draw/`（偏移链 + 每帧遍历 + 绘制）、`计算/`（W2S）、`Android_touch/`（uinput 自瞄触控）、`native_surface/`（窗口获取）
- 头文件与实现分离：接口集中 `include/`、实现集中在 `src/`；第三方统一放 `third_party/` 按 git 管理，跨系统版本差异由自研 `native_surface` 内部按 `ro.build.version.release` 分派
- 注入式变体才需要 `Canvas/ / GUI/ / UE_SDK/` 这类功能目录；本文档为独立 ELF 外部方案，不涉及

### 9.1 必要第三方依赖清单

按本文档主线（独立 ELF + 驱动 ioctl + ANativeWindow + ImGui/Vulkan）提取，缺一不可的核心库与按需选用的可选库：

| 库 | 用途 | 必要性 | 版本 | 说明 |
| --- | --- | --- | --- | --- |
| **ImGui** | UI 绘制（方框 / 血条 / 名字 / 菜单） | 必需 | v1.92.9b | 核心 `imgui.cpp / imgui_draw.cpp / imgui_tables.cpp / imgui_widgets.cpp`；后端 `imgui_impl_vulkan`（见 10.3 ②）。**不需要** `imgui_impl_android` 输入后端——纯 ELF 无输入事件来源，自瞄走 uinput |
| **ANativeWindowCreator（自研最小符号集）** | 无 App 环境下创建 `ANativeWindow` | 必需 | 自研 | dlopen `libgui.so`/`libutils.so` + dlsym 必需 ~24 个 mangled 符号直连 SurfaceFlinger 建 layer，支持 Android 11~16；全部符号名 / 库路径经 Encrypt.h 编译期加密，无公开指纹（见 10.3 ①） |
| **libvulkan.so** | Vulkan API | 必需（系统库） | 系统自带 | `dlopen` 动态加载（见 10.3 ②），非第三方，无需打包 |
| cJSON | 配置文件 / 协议解析 | 可选 | v1.7.19 | 设置持久化、远程配置下发 |
| obfuscate.h / StrEnc / Encrypt.h | 编译期 / 运行时字符串加密 | 可选 | - | 防明文特征串落盘，见 11.1 |
| FreeType（libfreetype.a）+ 中文字体 | ImGui 中文字体渲染 | 可选 | - | 需中文界面时引入；配合 `zh_Font.h` / fontawesome 图标 |

> 不必要：KittyMemory / shadowhook / `raw_syscall`（syscall.s）等是**注入式**反 hook 工具链，本文档为独立 ELF 外部方案，无需引入。

## 10. 安卓平台：纯 ELF 外部绘制方案

### 10.1 总体架构

**单一完整技术路线**：纯 ELF 可执行二进制（无 App、无注入、无悬浮窗权限）。**一个进程**完成全部工作：驱动 ioctl 每帧读游戏内存 → W2S → ImGui 绘制 → **ANativeWindow + Vulkan** 上屏。

```
游戏进程 ◄──内核驱动 ioctl── 本 ELF（esp，/data/local/tmp/，root 运行）
                             │
                             ├─ 每帧：读 GWorld 链 / 相机矩阵 / Actor 字段
                             ├─ W2S 投影 → ImGui 绘制（方框/血条/名字）
                             └─ ImGui 绘制 → Vulkan（vkQueuePresentKHR）→ ANativeWindow → SurfaceFlinger 上屏
```

### 10.2 运行准备

- 形态：**单一独立 ELF 可执行文件**，root（Magisk）下直接运行，如 `/data/local/tmp/esp`
- **不注入游戏进程**，跨进程读取统一走内核驱动 ioctl（见 2.1 节）：驱动节点自动发现 + 用完即删
- 游戏 PID 从 `/proc` 按进程名匹配获取；模块基址直接由驱动 `OP_MODULE_BASE` 返回 `libUE4.so` 地址，无需解析 maps
- 偏移表（GWorld / 世界链 / 相机矩阵 / Actor 字段）集中放 `Offsets` 命名空间（见 2.2 节），驱动打开一次全程复用

### 10.3 Overlay 绘制端（ANativeWindow + ImGui + Vulkan）

渲染链路：**ANativeWindow（native_surface 提供）+ ImGui + Vulkan**。单一 ELF 进程，无 Activity / 无悬浮窗权限。

> **先分清两个概念（易混）：**
> - **ANativeWindow**：Android NDK 官方类型，一个"可绘制的窗口句柄"，代表一块能画的 Surface。Vulkan / EGL 都拿它当渲染目标（`vkCreateAndroidSurfaceKHR` / `eglCreateWindowSurface`），画完的内容最终交给 SurfaceFlinger 合成上屏。它只是"目标"，自己不会凭空产生。
> - **native_surface**：第三方库（非系统 API）。解决的是：纯 ELF 进程里**没有 Activity、没有 Java Surface**，拿不到常规的 ANativeWindow，所以它绕开 App 体系，直接用 Binder 连 SurfaceFlinger 创建 layer / Surface，再封装返回一个 `ANativeWindow*`。**它是"获取 ANativeWindow 的手段"，ANativeWindow 是"拿到手的结果"**。
> - 类比：ANativeWindow 是"门"（渲染目标），native_surface 是"造门的人"（无 App 环境下拿到门的方法），Vulkan 是"画笔"。

- 每帧逻辑：ioctl 读 Actor（第 4 节）→ W2S 投影（第 3 节）→ 按第 6 节绘制方框 / 血条 / 名字

**① ANativeWindow 来源：自研最小符号集直连 SurfaceFlinger（唯一路线）**

- 纯 ELF 进程没有 Activity / Java Surface，拿不到常规 `ANativeWindow`；因此绕开 App 体系，直接 `dlopen("/system/lib64/libgui.so" + "/system/lib64/libutils.so")` + `dlsym` 各版本 mangled 符号，调 `SurfaceComposerClient::createSurface` 建 layer / Surface，封装返回 `ANativeWindow*`
- **反特征设计**（对应 11 章）：不引入任何第三方窗口库（公开指纹风险）；只 dlsym 创建 / 置顶 / 防录屏必需的 ~24 个符号（比开源库的 60+ 少一半），按 `ro.build.version.release`（Android 11~16）条件解析
- **符号名 / dlopen 路径 / layer 名全部经 Encrypt.h 编译期 XOR 加密**，运行时解密后 dlsym，二进制无明文特征串
- 无日志、无异常、无内嵌 so / memfd 执行；sp 引用计数手管（`RefBase::incStrong/decStrong`），无泄漏式常驻
- 调用方式：`CreateNativeWindow({.width=0, .height=0})`，宽高传 0 自动取当前主屏；`GetScreenSize(w, h)` 取实际屏幕尺寸供 W2S 用
- 防录屏内置：Android 11 用 `LayerMetadata::setInt32(2, 441731)`；Android 12+ 用窗口 flags `0x40`（eSkipScreenshot）+ `setTrustedOverlay` + `setLayer(INT_MAX)`
- **Android 16 适配**：16 的 `createSurface` / `Transaction::apply` 沿用 Android 14 起的分支签名（`g_ver >= 14` 自动覆盖 14~16），无需特判；Android 16 唯一变化的 `setInputWindowInfo` 是触摸相关 API，本方案用 uinput 自瞄不依赖它。版本上限不设死：17+ 自动回退 14 分支尝试，dlsym 失败即安全失败（建窗返回空，不崩溃）
- 屏幕尺寸查询为可选能力（`getDisplayState` 解析失败仅退回调用方尺寸/默认 1080×2400），不会拖垮建窗

**② ImGui + Vulkan：外部 overlay 渲染（自有 swapchain）**

- `dlopen("libvulkan.so")` + `ImGui_ImplVulkan_LoadFunctions` 动态加载 Vulkan 函数，无需静态链接
- 建 Instance：启用 `VK_KHR_surface` + `VK_KHR_android_surface` 扩展
- 选物理设备（优先独立 GPU）→ 图形队列族 → 建逻辑设备（启用 `VK_KHR_swapchain`）→ 建 DescriptorPool
- `vkCreateAndroidSurfaceKHR(instance, {.window = win})` 用 `ANativeWindow*` 建 surface
- 选 surface format / present mode → `ImGui_ImplVulkanH_CreateOrResizeWindow` 建 swapchain / renderpass / framebuffer
- `ImGui_ImplVulkan_Init` 后每帧：`NewFrame` → 绘制 → `RenderDrawData` → `vkQueuePresentKHR`
- 返回 `VK_ERROR_OUT_OF_DATE_KHR` 时重建 swapchain（屏幕旋转 / 切后台）

> 上述 ① 自研实现即按系统版本 dlsym libgui.so 的 mangled 符号，无需自写版本适配；此处只需把返回的 `ANativeWindow*` 交给 `vkCreateAndroidSurfaceKHR`。
>
> ② 的完整代码已落地：`src/main.cpp` 的 `InitVulkanImGui`（上述 7 步）与 `PresentFrame`（`NewFrame → TickFrame → Render → FrameRender → FramePresent`，含 swapchain 过期重建）；`src/Android_draw/draw.cpp` 的 `DrawBox/DrawHealth/DrawName` 经 `ImGui::GetBackgroundDrawList()` 绘制。

**防录屏 / 防截屏（已内置于 CreateNativeWindow）：**

- Android 11：创建 layer 时 `LayerMetadata::setInt32(2, 441731)`（跳过截屏）
- Android 12+：窗口 flags 加 `0x40`（eSkipScreenshot）+ `Transaction::setTrustedOverlay(true)` + `setLayer(INT_MAX)` 置顶

### 10.4 每帧渲染循环

- 单线程主循环：`OpenDriver()` → 每帧 ioctl 读数据 → W2S + 绘制 → `vkQueuePresentKHR` 提交 → 下一帧
- 驱动读内存极快（单次 ioctl 数十微秒），每帧遍历无性能压力，实时性最好
- 无共享内存、无定时器：数据读取与绘制同帧同步，天然无滞后

```cpp
// 主循环（main.cpp）
int drv = OpenDriver();                    // 驱动节点，用完即删
ANativeWindow* win = CreateNativeWindow();     // 自研 native_surface 提供（宽高 0 = 自动取屏幕）
GetScreenSize(W, H);                           // 取实际屏幕尺寸供 W2S
// Vulkan + ImGui 初始化（Instance/Swapchain/DescriptorPool + ImGui_ImplVulkan_Init）...

while (g_Running) {
    uintptr_t world, level, data;
    int32_t   count;
    FMatrix   viewProj;
    if (!Read(drv, pid, g_Base + Offsets::GWorld, world))                       continue;
    if (!Read(drv, pid, world + Offsets::PersistentLevel, level))               continue;
    if (!Read(drv, pid, level + Offsets::Actors, data))                         continue;
    if (!Read(drv, pid, level + Offsets::ActorCount, count))                    continue;
    Read(drv, pid, g_Camera + Offsets::ViewProjection, viewProj);               // 相机矩阵每帧读

    for (int32_t i = 0; i < count; i++) {
        uintptr_t actor;
        if (!Read(drv, pid, data + i * sizeof(uintptr_t), actor) || !actor) continue;
        FVector pos; float hp, maxHp;
        Read(drv, pid, actor + Offsets::RootLocation, pos);
        Read(drv, pid, actor + Offsets::Health, hp);
        Read(drv, pid, actor + Offsets::MaxHealth, maxHp);

        ImVec2 scr;
        if (!WorldToScreen(pos, viewProj, W, H, scr)) continue;   // 视锥外跳过
        DrawESP(pos, hp, maxHp, scr);      // 方框 / 血条 / 名字
    }
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd); // 提交绘制命令
    vkQueuePresentKHR(queue, &present);   // 提交给 SurfaceFlinger 上屏
}
```

### 10.5 安卓特有坑点

| 现象 | 原因 / 解决 |
| --- | --- |
| 图层不显示 | layer 未 attach 到 display 或层级过低；需 `setLayer` 置顶 |
| 读内存失败 | SELinux / 权限不足；确认 root、驱动节点权限及 ioctl 可用 |
| 数据与画面错位 | 相机矩阵未每帧重读，W2S 矩阵过期；确保每帧更新 viewProj |
| SurfaceFlinger 版本差异 | 不同安卓版本 Binder 接口 / `libgui` 符号变化，需按系统版本适配 |
| 角色名 / 类名读不到 | 游戏 GNames 加密数组；扫描 maps 找 TArray 头特征回退定位解密数组 |
| 读内存崩溃 | 地址未驻留触发缺页；用 `mincore` 预检页驻留（过缺页），读前校验地址区间与 4 字节对齐 |
| 录屏/截图看不到 Overlay | 未设置跳过截屏标志（441731 / flags 0x40）；按系统版本添加 |
| 被反作弊检测 | 跨进程读内存行为易被扫描，仅建议技术研究 |

## 11. 安全与反检测（进阶）

独立 ELF 外部绘制方案可叠加的通用防护手段，与具体实现无关，按需取舍。

### 11.1 字符串与数据加密

- 编译期字符串加密：`obfuscate.h`（XOR + murmur 散列），避免明文特征串落盘
- 运行时加密：`StrEnc` / RC4 随用随解密，敏感字符串不常驻内存

### 11.2 内存读取加固

- 防缺页：读前 `mincore` 预检页驻留 + 地址区间 / 4 字节对齐校验，避免读到未驻留页触发信号
- 加密名字数组（部分游戏 GNames 加密）：扫描 `/proc/pid/maps` 匿名段，按 TArray 头特征（size 100~150 + 连续 0）回退定位解密数组

### 11.3 自瞄触控（uinput 虚拟触摸）

- 注册 uinput 虚拟触摸设备：随机设备名 / 随机 PHYS / 复制原设备 keybits 与 EVIOCGID，模拟手指注入
- `EVIOCGRAB` 独占真实触摸屏后由 TypeA 线程透传到虚拟设备（游戏输入不中断），自瞄用独立虚拟手指（TRACKING_ID 199）注入屏幕坐标吸附目标，`TouchSetOrientation(0/1/2/3)` 处理横屏旋转映射，绕过游戏手势校验

### 11.4 进程与运行环境反检测

- 驱动节点自动发现 + 用完即删（见 2.1 节），不留静态文件供扫描
- 环境检测：`LD_PRELOAD` / 注入模块 / 反调试 / VPN 与 iptables 重定向；`chmod 000 /proc/bus/input/*` 阻断输入读取
- 形态优势：独立 ELF 无包名、无签名、无 Activity，比注入式 so 隐蔽得多

### 11.5 为什么不采用注入式（对比说明）

- 注入式（so 注入 + Hook `UCanvas` 虚表槽 106 走 UE4 内置绘制）渲染帧同步，但需进游戏进程、改其内存与虚表，反作弊特征明显
- 本方案全部工作在进程外（读内存走驱动、绘制走 SurfaceFlinger），游戏进程零改动
- 取舍：稳定与隐蔽优先于帧同步，故不采用注入式

> 注意：跨进程读内存行为本身仍可能被扫描，本文档仅作技术研究；游戏版本更新后偏移全部失效，需按第 7 节重新扫描。

