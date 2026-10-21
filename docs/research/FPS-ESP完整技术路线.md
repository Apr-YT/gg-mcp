# FPS ESP 完整技术路线（数据 → 结构 → 代码）

> 调研整理（2026-08-18，来源：UnknownCheats / CSDN / GitHub 教程 / 外挂圈技术贴）
> 适用于授权安全研究/游戏安全对抗分析。渲染层方案与 GG-Ghost 项目能力对照见文末。

---

## 一、数据面：ESP 需要哪 4 组数据

| # | 数据 | 用途 | 典型形态 |
|---|---|---|---|
| 1 | **玩家实体列表**（EntityList / GObjects） | 遍历所有玩家，逐个取属性 | 指针数组/链表（Source 引擎步进 0x10/0x40；UE 为 GObjects 全局对象表） |
| 2 | **玩家坐标**（Origin / 位置） | 3D 世界坐标 | `Vector3 {float x,y,z}`（连续 3 float），骨骼则为 boneMatrix `0x30*idx + 0x0C/0x1C/0x2C` |
| 3 | **视角矩阵**（View Matrix） | 世界坐标 → 屏幕坐标投影 | `4x4 float[16]`（每帧更新；UE4 周围有 0F 填充） |
| 4 | **本地玩家/相机指针** | 确定"观察者"与矩阵归属 | 基址偏移链：`GameBase+0x2F4A30 → +0x1C → +0x90` |

附加（进阶）：玩家血量、阵营、名字、是否死亡（过滤用）、FOV、瞄准方向。

---

## 二、定位方法（CE 实操，怎么找）

### 2.1 找实体列表（数组基址）——"What accesses" 法
1. 先定位任一样本数据（如 HP 地址）
2. `Find out what accesses this address` → 观察汇编 `mov eax, [reg + index*4]`
3. 断点执行观察寄存器：**index 每次 +0x10/+0x40 递增** = 数组访问特征
4. 由 `base + index*step + 属性偏移` 反推 → **数组基址 + 结构步长**
5. CE `Dissect data structures` 粘贴地址自动还原结构体字段

### 2.2 找坐标
- 玩家位置 = 3 个连续 float（xyz）——移动角色观察值变化确认
- 精确做法：骨骼（boneMatrix）——跳跃/下蹲时脚底坐标飘移，骨骼点始终贴模型

### 2.3 找视角矩阵（View Matrix）——未知值 + Changed 反复过滤
1. 附加进程 → **Unknown initial value** + Float 类型扫描
2. 游戏中**旋转视角** → CE 选 **Changed value** 再扫
3. 重复多次（转视角→扫 Changed→静止→扫 Unchanged）直到 <1000
4. 在剩余候选里找 **16 个连续 float** 的内存块

**真矩阵验证特征**：
- 视角转动才变（静止不动矩阵不变；开镜/关镜矩阵头变化）
- 16 个连续 float（完整 4x4）
- 旋转部分近正交：`dot(row0,row1)≈0`，行向量模≈1
- UE4 矩阵周围**必有 0F 填充**

**假矩阵识别**（反作弊干扰项"影子矩阵"）：
- 转视角所有值都在变（无规律随机波动 vs 真矩阵抛物线趋势）
- 数值超范围（>10000 / <0.0001）
- 结构不完整（不足 16 个 float / 无 0F 分隔）

### 2.4 找本地玩家/相机
由矩阵偏移链反查：找到矩阵 → 谁的字段引用它 → 逐级上溯到模块基址 → 得到静态偏移链

---

## 三、结构面：还原目标结构

```c
struct Vector3 { float x, y, z; };

struct Matrix4x4 { float m[16]; };   // 行主序: m[0..3]=第0行...

// 实体（玩家）结构（示例，偏移因游戏而异）
struct Entity {
    Vector3  origin;        // +0x14C  脚底坐标（Apex 例）
    int      health;        // +0x..   血量
    int      team;          // +0x..   阵营
    char     name[32];      // +0x..   名字
    uintptr  boneMatrix;    // +0x..   骨骼矩阵指针
};
```

偏移链形式：`GameBase + 0x18DA338 (entitylist) → 0x14C (origin)`、`GameBase + 0x1B3BD0 (matrix)`（Apex 实例）。

---

## 四、代码面：从 W2S 到绘制

### 4.1 WorldToScreen（核心算法，各引擎大同小异）

```cpp
// pos: 世界坐标; matrix: 视图投影矩阵（16 float）
Vector3 WorldToScreen(const Vector3& pos, const Matrix4x4& matrix) {
    float _x = matrix.m[0]*pos.x + matrix.m[1]*pos.y + matrix.m[2]*pos.z + matrix.m[3];
    float _y = matrix.m[4]*pos.x + matrix.m[5]*pos.y + matrix.m[6]*pos.z + matrix.m[7];
    float w  = matrix.m[12]*pos.x + matrix.m[13]*pos.y + matrix.m[14]*pos.z + matrix.m[15];
    if (w < 0.001f) return {-9999,-9999,0};      // 在镜头后方, 剔除
    _x *= 1.f / w;  _y *= 1.f / w;                // 透视除法
    Vector3 out;
    out.x = (float)width  * .5f + 0.5f * _x * width  + 0.5f;
    out.y = (float)height * .5f - 0.5f * _y * height + 0.5f;   // 注意 Y 翻转
    out.z = w;                                    // 距离, 用于可见性/距离绘制
    return out;
}
```

### 4.2 主循环（每帧/每 tick）

```
1. 读 ViewMatrix（4x4）
2. for i in 实体列表:
     读 origin（脚底）+ boneMatrix（头部/骨骼点）
     读 health/team → 过滤（死亡/己方跳过）
     WorldToScreen(foot) / WorldToScreen(head)
     画 box（宽=高/1.2）/ 骨骼线 / 血条 / 距离文字
```

### 4.3 渲染方案（两条主流路线）

| 方案 | 原理 | 隐蔽性 | 反检测面 |
|---|---|---|---|
| **外部 Overlay** | 透明置顶窗口 + GDI/DirectX/ImGui 绘制，不注入游戏 | 高（进程隔离，无注入痕迹） | Overlay 窗口本身可被枚举 |
| **内部 Hook** | 注入 DLL，Hook 游戏绘制函数（Present/绘制链） | 低-中（有注入痕迹） | 模块列表/线程注入检测 |

### 4.4 进阶绘制
- **骨骼 ESP**：读 boneMatrix → 多骨骼点连线（0x30 步长 12 个标准骨）
- **距离/血量**：w（clip z）换算距离；血量值画血条
- **平滑/预判**（自瞄向）：瞄准点平滑插值、弹道预判

---

## 五、对抗面（游戏反作弊手段 vs 对策）

| 反作弊手段 | 对策 |
|---|---|
| **加密矩阵**（XOR） | 找解密函数 hook 点 / 内存写入断点 / XMM 寄存器监控浮点操作；CE Lua 写解密器 |
| **动态矩阵地址**（每局变） | AOB 特征码扫描（定位矩阵特征字节）+ 指针链动态解析 |
| **影子矩阵干扰** | 正交验证 + 抛物线变化趋势验证 + 0F 填充特征 |
| **进程调试检测**（TracerPid） | 内存读写走驱动（不 ptrace）——GG-Ghost 内核通道天然规避 |
| **模块/驱动枚举** | 模块隐藏（rwproc33 CMD_HIDE_KERNEL_MODULE） |
| **Overlay 窗口枚举** | 透明窗口伪装/硬件 cursor 绘制 |

> **坐标加密专项**（7 种加密形式 + 识别特征 + 解密破解链 + 蜜罐识别 + 防守检测面）→ 见 [坐标加密对抗方法.md](坐标加密对抗方法.md)

---

## 六、适配 GG-Ghost 项目（我们已有能力的落点）

| ESP 环节 | 用我们什么 |
|---|---|
| 读实体列表/坐标/矩阵 | `gg_read`（**内核通道，零 TracerPid**） |
| 找矩阵/数组（AOB 特征码） | `gg_search type="bytes"`（AoB，刚实现） |
| 找偏移链/结构 | `gg_pointer_scan` / `gg_pointer_chain` / `gg_disassemble` |
| 矩阵/坐标变化确认 | `value_track.py`（值追踪）+ `memdiff.py`（内存 diff） |
| 断点追"谁更新矩阵" | `gg_watch_set`（全线程）+ 命中反汇编 |
| 渲染 | PC 侧 Overlay（ImGui/DirectX），数据经 MCP 8788 读取 |

**落地形态**：设备端 GG-Ghost 只负责读内存（内核驱动）→ MCP 工具暴露 → PC 端 Python/C++ 脚本做 W2S + Overlay 绘制。ESP 数据面（找矩阵/实体/偏移）全流程可用我们现有工具链完成。

---

*本路线仅用于授权安全研究与防御分析。*
