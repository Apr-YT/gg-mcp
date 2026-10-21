# CF ESP 代码框架（dump.cs 结构 + 运行时偏移，2026-08-20）

> 授权安全研究/防御分析用对抗样本框架。数据面基于 `cf_runtime_offsets.tsv`（302515 字段偏移，运行时提取）与 dump.cs 类结构。
> 状态：**结构/偏移已定死，对象基址待实测注入**（m_NickName 锚定法）。
> ⚠️ 铁律：**只读直读（内核通道），绝不写/冻结**；字符串搜索+指针反查有封号前科（08-18 封 7 天），操作要少而精准。

---

## 一、数据面（已验证偏移，直接可用）

### 1.1 对象入口链

```
EntityStateSyncManager(15127)          LogicalEntityCenter(15132)
  m_localPawnPos @ 0x3c  ← 本地坐标     m_EntityDic @ 0x18 ← 实体字典
  m_ThreadEntitys @ 0x18 ← 实体列表      (Dictionary<int,LogicalEntity>)
  m_viewPos      @ 0x30  ← 视图位置
```

### 1.2 实体结构（LogicalEntity 基类 → PawnEntity 派生）

```
LogicalEntity(15131):
  +0x10 EntityType        (u32)
  +0x18 EntityID          (u64)
  +0x20 IsRelevant        (u8)
  +0x24 Position          (Vector3, 12B)   ← 实体坐标
  +0x30 Rotation          (Vector3, 12B)
  +0x3c IsPendingRemove   (u8)
  +0x40 DataIndex         (u32)

PawnEntity(15134):  // : LogicalEntity，字段续排
  +0x44 Camp              (u32)             ← 阵营过滤
  +0x48 RoleID            (u32)
  +0x50 RespawnType       (u32)
  +0x54 RespawnTime       (u32)
  +0x58 IsAlive           (u32)             ← 死亡过滤
  +0x5c LastServerRespawnTime (u32)
  +0x78 Data              (引用)
```

### 1.3 本地玩家

```
LocalPlayer(15090):
  +0x18 m_NickName        (String 引用)     ← 基址锚定用（搜昵称字符串→反查指针）
  +0x20 m_Camp            (u32)
  +0x28 m_PlayerController(引用)
```

### 1.4 ViewMatrix —— 读取公式已实锤（对象地址待定）

- `UnityEngine.Camera` C# 实例字段只有事件（onPreCull@0/onPreRender@0x8/onPostRender@0x10），**矩阵在 native Camera 对象**（il2cpp 的 worldToCameraMatrix/projectionMatrix 是方法，矩阵存 C++ 侧）
- **2026-08-20 运行时逆向实锤（libunity.so）**：
  - **native Camera = C# Camera 对象 + 0x10**（`0x789b8300e4` 转换函数 `ldr x0,[x0,#0x10]` = m_CachedPtr）
  - **native Camera + 0x48 = 64B 矩阵副本**（`0x789b398dc4` 拷贝：源 `[x1+0x0..0x30]` → 目标 `[x0+0x48..0x78]`）
  - **native Camera + 0x358 = 矩阵缓存区**（w2cm 核心 `0x789b39cc38` memcpy 源），+0x350 版本号 / +0x398 矩阵索引
  - **libunity GOT（0x789c24xxxx）可读** → 真实函数地址可解析
  - 读取公式：`Camera对象 + 0x10 → native Camera → +0x48 或 +0x358 读 64B 矩阵`
- **⚠️ 卡点（对象定位）**：C# Camera 对象地址未锚定。CF 防读面：libunity 数据区（0x789c42~0x789c45）PTE 拦截、懒绑定槽防读、矩阵非标准布局（投影矩阵特征 idx14=-1.0 搜索被堆对象结构干扰）
- **✅ 替代方案：角度法 W2S（FPS 不需要矩阵！）**——已确认 CF 是 FPS + FOV=55°（PlayerController+0x1d8 实测）：
  ```
  屏幕X = 屏宽/2 + (敌yaw - 己yaw) / (FOV/2) × (屏宽/2)
  屏幕Y = 屏高/2 + (敌pitch - 己pitch) / (FOV/2) × (屏高/2)
  ```
  只需：自己坐标 + 自己视角(yaw/pitch) + 敌人坐标 + FOV + 分辨率。yaw/pitch 在玩家控制对象（比 Camera 好定位）
- 矩阵语义（若走矩阵路线）：Unity 是 **Column-Major**，W2S 用 `row = M[col*4+row]` 取行

---

## 二、结构体定义（C++，偏移已核）

```cpp
#include <cstdint>
#include <string>
#include <vector>

// ---------- 基础 ----------
struct Vec3 { float x, y, z; };

// 4x4 矩阵（Unity 列主序，m[col*4+row]）
struct Mat4x4 { float m[16]; };

// ---------- 读取层（只读，走内核驱动 gg_read） ----------
// 按游戏对象基址 + 偏移读任意字段；CF 铁律：只读不写
uint64_t  R64(uint64_t base, uint32_t off);       // 读 8B 引用/指针
uint32_t  R32(uint64_t base, uint32_t off);       // 读 u32
uint8_t   R8 (uint64_t base, uint32_t off);       // 读 u8
Vec3      RV3(uint64_t base, uint32_t off);       // 读 12B 向量
std::string RStr(uint64_t base, uint32_t off);    // 读字符串（NickName 锚定用）

// ---------- 结构偏移（cf_runtime_offsets.tsv 实锤） ----------
namespace CF {
    constexpr uint32_t ESSM_THREAD_ENTITYS = 0x18;  // EntityStateSyncManager.m_ThreadEntitys (List)
    constexpr uint32_t ESSM_LOCAL_PAWN_POS = 0x3C;  // EntityStateSyncManager.m_localPawnPos (Vec3)
    constexpr uint32_t LEC_ENTITY_DIC      = 0x18;  // LogicalEntityCenter.m_EntityDic (Dictionary)

    constexpr uint32_t LE_ENTITY_ID    = 0x18;
    constexpr uint32_t LE_IS_RELEVANT  = 0x20;
    constexpr uint32_t LE_POSITION     = 0x24;  // Vec3
    constexpr uint32_t LE_ROTATION     = 0x30;  // Vec3

    constexpr uint32_t PE_CAMP    = 0x44;
    constexpr uint32_t PE_ROLE_ID = 0x48;
    constexpr uint32_t PE_IS_ALIVE= 0x58;

    constexpr uint32_t LP_NICKNAME  = 0x18;  // LocalPlayer.m_NickName
    constexpr uint32_t LP_CAMP      = 0x20;
    constexpr uint32_t LP_CONTROLLER= 0x28;
}

// ---------- il2cpp 容器布局（标准结构） ----------
// List<T>:  { T* _items(Array); int _size; int _version; }
//   List @0x0:  klass@0 monitor@0x8 _items@0x10 _size@0x18 _version@0x1c
//   Array    :  klass@0 monitor@0x8 length@0x10 data@0x18
struct ListHeader { uint64_t klass; uint64_t monitor; uint64_t items; int32_t size; };
struct ArrayHeader{ uint64_t klass; uint64_t monitor; int32_t length; };

// Dictionary<K,V>: { int* buckets; Entry[] entries; int count; ... }
//   Entry: { int hashCode; int next; K key; V value; }   (泛型内联)
struct DictHeader { uint64_t klass; uint64_t monitor; uint64_t buckets; uint64_t entries; int32_t count; };

// ---------- 游戏对象基址（运行时注入，见第四节锚定法） ----------
struct CF_Base {
    uint64_t entityStateSyncMgr;   // EntityStateSyncManager 实例
    uint64_t localPlayer;          // LocalPlayer 实例（可选）
    uint64_t worldCamera;          // UnityEngine.Camera 实例（矩阵 TODO）
};
```

---

## 三、核心算法

### 3.1 WorldToScreen（Unity 列主序）

```cpp
// matrix: native Camera 的 view*proj（列主序）。Unity 世界坐标 → 屏幕
bool WorldToScreen(const Vec3& world, const Mat4x4& m, int sw, int sh, float& sx, float& sy, float& dist) {
    // 列主序取行：row_i = { m[0*4+i], m[1*4+i], m[2*4+i], m[3*4+i] }
    float cx = m[0]*world.x + m[4]*world.y + m[8]*world.z + m[12];
    float cy = m[1]*world.x + m[5]*world.y + m[9]*world.z + m[13];
    float cw = m[3]*world.x + m[7]*world.y + m[11]*world.z + m[15];
    if (cw < 0.001f) return false;                 // 镜头后方剔除
    float inv = 1.f / cw;
    float nx = cx * inv, ny = cy * inv;            // NDC
    sx = (sw * 0.5f) + nx * sw * 0.5f;
    sy = (sh * 0.5f) - ny * sh * 0.5f;             // Y 翻转
    dist = cw;
    return true;
}
```

### 3.2 实体遍历 —— 优先 List（m_ThreadEntitys），字典兜底

```cpp
// 路线 A：EntityStateSyncManager.m_ThreadEntitys 是 List<LogicalEntity>（引用数组）
std::vector<uint64_t> ReadEntityList(uint64_t essm) {
    std::vector<uint64_t> out;
    uint64_t list = R64(essm, CF::ESSM_THREAD_ENTITYS);
    if (!list) return out;
    ListHeader lh = ReadT<ListHeader>(list);
    if (!lh.items || lh.size <= 0 || lh.size > 4096) return out;   // 上限保护
    ArrayHeader ah = ReadT<ArrayHeader>(lh.items);
    int n = (ah.length < lh.size) ? ah.length : lh.size;
    for (int i = 0; i < n; i++)
        out.push_back(R64(lh.items + 0x18 + (size_t)i * 8));       // 元素引用
    return out;
}

// 路线 B（兜底）：LogicalEntityCenter.m_EntityDic 是 Dictionary<int,LogicalEntity>
// entries 元素 = { hashCode u32, next u32, key u32(填充), value u64 } = 24B
std::vector<uint64_t> ReadEntityDict(uint64_t lec) {
    std::vector<uint64_t> out;
    uint64_t dict = R64(lec, CF::LEC_ENTITY_DIC);
    if (!dict) return out;
    DictHeader dh = ReadT<DictHeader>(dict);
    if (!dh.entries || dh.count <= 0 || dh.count > 4096) return out;
    for (int i = 0; i < dh.count; i++) {
        uint64_t e = dh.entries + 0x18 + (size_t)i * 24;
        uint64_t val = R64(e, 16);                 // Entry.value @ +16
        if (val) out.push_back(val);
    }
    return out;
}
```

### 3.3 主循环（每帧）

```cpp
void EspTick(CF_Base& b, int sw, int sh) {
    // 1. 读矩阵（TODO：ViewMatrix 定位后实现）
    Mat4x4 vp = ReadViewMatrix(b.worldCamera);     // TODO
    // 2. 本地坐标（过滤用：距离）
    Vec3 localPos = RV3(b.entityStateSyncMgr, CF::ESSM_LOCAL_PAWN_POS);
    // 3. 遍历实体
    for (uint64_t ent : ReadEntityList(b.entityStateSyncMgr)) {
        if (!ent) continue;
        if (!R8(ent, CF::LE_IS_RELEVANT)) continue;
        uint32_t camp = R32(ent, CF::PE_CAMP);     // 阵营（PawnEntity）
        uint32_t alive = R32(ent, CF::PE_IS_ALIVE);
        if (camp == myCamp || !alive) continue;    // 过滤
        Vec3 pos = RV3(ent, CF::LE_POSITION);
        float sx, sy, dist;
        if (!WorldToScreen(pos, vp, sw, sh, sx, sy, dist)) continue;
        DrawBox(sx, sy, dist);                     // 绘制（Overlay 层）
    }
}
```

---

## 四、对象基址定位（m_NickName 锚定法，⚠️ 有封号风险）

日志 08-18 定位方案（未实测）：

```
1. gg 搜本地玩家昵称字符串（bytes）→ 得到字符串地址 S
2. gg 搜 qword=S（指针反查）→ 命中处即引用 S 的地方
3. 从命中地址 -0x18 即可能为 LocalPlayer 实例基址（m_NickName @ 0x18）
4. 验证：+0x28 m_PlayerController 读出的指针能解引用 → 确认
5. EntityStateSyncManager 实例：沿 m_PlayerController 或另锚定
```

**风险标注**：08-18 封号（7 天）的触发动作正是"昵称字符串搜索 + qword 反查"。若本账号已解封，**操作前权衡**；优先改为：
- 只读直读已初始化对象（不做字符串搜索）：进对局后先试 `m_localPawnPos` 特征（本地玩家坐标浮点变化）定位 ESSM
- 或复用 08-19 已验证的 Il2CppClass 链（FieldInfo 反查，只读）替代字符串反查

---

## 五、待办清单（按序）

| # | 项 | 方法 | 依赖 |
|---|---|---|---|
| 1 | **ViewMatrix 定位** | r2 反汇编 Camera::worldToCameraMatrix / 运行时矩阵特征搜索 | 游戏开着（主界面即可） |
| 2 | **ESSM 实例基址** | m_localPawnPos 浮点变化法 / NickName 锚定（风险） | 进对局 |
| 3 | **实体列表验证** | 读 m_ThreadEntitys 长度/元素，对照对局人数 | 进对局 |
| 4 | 完整跑通 | 矩阵 + 基址 + 列表三合一 | 1-3 完成 |

---

*对抗样本框架，仅限授权安全研究/防御分析。*
