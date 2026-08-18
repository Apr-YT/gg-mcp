# 《FPS矩阵绘制算法原理》全合集详解

> 博主：探探讲逆向
> 合集：2026 FPS矩阵绘制算法原理 手把手带你写矩阵画方框
> 共 11 集，总时长约 4 小时 15 分钟

---

## 合集大纲

### 第一阶段：基础准备
| 集数 | 标题 | 时长 | 核心内容 |
|------|------|------|----------|
| 1.0 | 课程必备知识说明 | 07:25 | 前置知识、学习路线、工具准备 |
| 2.0 | 寻找所有人物数据 | 32:17 | 内存逆向、定位玩家/敌人数据结构 |
| 3.0 | 编写绘制代码框架 | 41:31 | 搭建绘制项目骨架、D3D/Overlay 框架 |

### 第二阶段：矩阵数学基础
| 集数 | 标题 | 时长 | 核心内容 |
|------|------|------|----------|
| 4.1 | 认识矩阵 | 06:24 | 矩阵定义、4x4 矩阵结构、行/列主序 |
| 4.2 | 矩阵的乘法（绘制重点课） | 18:00 | 矩阵乘法运算、变换矩阵的复合 |
| 4.3 | 矩阵与3D图形变换关系 | 09:53 | 平移/旋转/缩放矩阵、模型变换原理 |

### 第三阶段：坐标变换与透视
| 集数 | 标题 | 时长 | 核心内容 |
|------|------|------|----------|
| 4.4 | 观察坐标系与世界坐标系 | 14:24 | 世界坐标→观察坐标、相机矩阵推导 |
| 4.5 | 寻找变换矩阵（重要） | 25:59 | 从游戏内存中定位 ViewMatrix / ProjectionMatrix |

### 第四阶段：绘制算法与实现
| 集数 | 标题 | 时长 | 核心内容 |
|------|------|------|----------|
| **4.6** | **矩阵绘制算法详解（重要）** | **18:05** | **W2S 世界转屏幕坐标、透视投影计算、方框绘制原理** |
| 4.7 | 更新函数封装 | 12:48 | 数据刷新、矩阵更新、线程封装 |
| 4.8 | 矩阵绘制代码实现 | 34:00 | 完整 ESP 方框绘制代码落地、调试与优化 |

---

## 1.0 课程必备知识说明（07:25）

### 课程目标
从零实现一个基于矩阵的 FPS 游戏透视绘制（ESP），核心是 **W2S（World-to-Screen）算法**——把游戏内 3D 世界坐标转换为屏幕 2D 坐标，从而在敌人头顶/身上画方框。

### 前置知识
| 领域 | 需要掌握的程度 |
|------|---------------|
| 线性代数 | 矩阵的基本概念、4×4 矩阵、乘法规则 |
| C/C++ 或易语言 | 能读写结构体、指针、内存操作 |
| 逆向基础 | CE（Cheat Engine）找地址、理解基址+偏移 |
| 图形学概念 | 了解 3D 坐标系、透视投影即可，课程会补 |

### 学习路线图
```
内存找数据 → 搭绘制框架 → 学矩阵数学 → 理解坐标变换 → 找变换矩阵 → 实现W2S算法 → 封装与优化
```

### 工具清单
- **Cheat Engine**：内存扫描、找基址偏移
- **ReClass.NET**：查看内存结构、生成结构体
- **Visual Studio / 易语言**：编写绘制程序
- **DirectX SDK / OpenGL**：绘制 API（Overlay 方式）

---

## 2.0 寻找所有人物数据（32:17）

### 核心目标
从游戏进程内存中定位：
- 本地玩家（LocalPlayer）坐标
- 敌人列表（EntityList）
- 每个实体的 3D 坐标（X, Y, Z）
- 生命值、队伍ID等筛选数据

### 逆向流程

#### 第一步：找本地玩家坐标
1. 用 CE 扫描当前 X 坐标（移动角色，数值变化）
2. 找到坐标地址后，**找出是什么访问了这个地址**
3. 回溯指针链，最终得到 `基址 + 偏移1 + 偏移2 + ... = 坐标地址`

#### 第二步：找实体列表
1. 观察多人游戏中，敌人坐标通常存放在一个**数组/链表**中
2. 常见结构：
```cpp
// 典型实体列表结构
struct Entity {
    Vec3 Position;   // +0x00 世界坐标
    Vec3 Velocity;   // +0x0C 速度
    int Health;      // +0x18 生命值
    int Team;        // +0x1C 队伍ID
    // ... 其他字段
};

// 实体列表
Entity* EntityList[64];  // 最多64人
```

#### 第三步：数据筛选逻辑
```
遍历实体列表 → 跳过本地玩家 → 跳过死亡实体(Health<=0) → 跳过队友(Team相同) → 保留有效敌人
```

### 关键技巧
- **坐标特征**：FPS 游戏中 Y 通常是高度轴，X/Z 是水平面
- **实体间距**：相邻实体在内存中的步长（Stride）是固定的
- **动态分配**：部分游戏实体是堆分配，需要通过指针数组间接访问

---

## 3.0 编写绘制代码框架（41:31）

### 两种绘制方案对比

| 方案 | 原理 | 优点 | 缺点 |
|------|------|------|------|
| **Hook 方式** | 注入游戏进程，Hook EndScene/Present | 与游戏同帧、无延迟 | 易被反作弊检测、开发复杂 |
| **Overlay 方式** | 顶层透明窗口覆盖游戏窗口 | 独立进程、安全、易调试 | 需要处理窗口同步、有微小延迟 |

### Overlay 框架结构

```
┌─────────────────────────────────────┐
│         透明顶层窗口 (WS_EX_LAYERED) │
│  ┌───────────────────────────────┐  │
│  │  DirectX/OpenGL 绘制表面       │  │
│  │  - 每帧读取游戏内存数据         │  │
│  │  - W2S 转换坐标               │  │
│  │  - 绘制方框/线条/文字          │  │
│  └───────────────────────────────┘  │
└─────────────────────────────────────┘
         ↓ 覆盖在游戏窗口上方
┌─────────────────────────────────────┐
│            游戏窗口                  │
└─────────────────────────────────────┘
```

### 核心框架代码（伪代码）
```cpp
// 主循环
while (running) {
    // 1. 更新游戏数据（读内存）
    UpdateGameData();
    
    // 2. 开始帧
    BeginFrame();
    
    // 3. 遍历所有敌人，绘制ESP
    for (each entity in EntityList) {
        if (IsValidTarget(entity)) {
            DrawESP(entity);  // 内部调用 W2S
        }
    }
    
    // 4. 结束帧，呈现
    EndFrame();
    
    // 5. 窗口消息处理
    ProcessMessages();
}
```

### 窗口同步要点
- 实时获取游戏窗口位置和大小，Overlay 窗口跟随移动
- 处理游戏全屏/窗口化切换
- 设置鼠标穿透（`WS_EX_TRANSPARENT`），不影响游戏操作

---

## 4.1 认识矩阵（06:24）

### 什么是矩阵
矩阵是一个按行和列排列的数字矩形阵列。在 3D 图形学中，我们几乎只用 **4×4 矩阵**。

```
┌            ┐
│ m00 m01 m02 m03 │
│ m10 m11 m12 m13 │
│ m20 m21 m22 m23 │
│ m30 m31 m32 m33 │
└            ┘
```

### 为什么是 4×4 而不是 3×3
因为需要表示**平移变换**。3×3 矩阵只能表示旋转和缩放（线性变换），无法表示平移。引入第 4 行/列（齐次坐标）后，平移也可以用矩阵乘法来表示，这样所有变换都能统一为"矩阵相乘"这一种操作。

### 齐次坐标
一个 3D 点 `(x, y, z)` 用齐次坐标表示为 `(x, y, z, 1)`，一个方向向量表示为 `(x, y, z, 0)`。
- w=1 表示点，可以平移
- w=0 表示方向，平移对它无效

### 行主序 vs 列主序
| 格式 | 内存布局 | 使用方 |
|------|---------|--------|
| 行主序 (Row-Major) | 按行连续存储 | DirectX、易语言多数实现 |
| 列主序 (Column-Major) | 按列连续存储 | OpenGL、多数数学库 |

**关键**：从游戏内存中读取矩阵时，必须搞清楚游戏用的是哪种存储顺序，否则乘法结果完全错误。

### 单位矩阵
主对角线为 1，其余为 0。乘以单位矩阵等于不做任何变换。
```
┌         ┐
│ 1 0 0 0 │
│ 0 1 0 0 │
│ 0 0 1 0 │
│ 0 0 0 1 │
└         ┘
```

---

## 4.2 矩阵的乘法（绘制重点课）（18:00）

### 乘法规则
两个矩阵 A（m×n）和 B（n×p）相乘，结果 C（m×p）：
```
C[i][j] = Σ(k=0→n-1) A[i][k] × B[k][j]
```

**核心要点**：
- 第一个矩阵的**列数**必须等于第二个矩阵的**行数**
- 矩阵乘法**不满足交换律**：A×B ≠ B×A
- 满足结合律：(A×B)×C = A×(B×C)

### 4×4 矩阵乘法示例
```cpp
Matrix4x4 Multiply(Matrix4x4 A, Matrix4x4 B) {
    Matrix4x4 C;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            C.m[i][j] = A.m[i][0]*B.m[0][j] +
                        A.m[i][1]*B.m[1][j] +
                        A.m[i][2]*B.m[2][j] +
                        A.m[i][3]*B.m[3][j];
        }
    }
    return C;
}
```

### 矩阵 × 向量（核心！）
这是 W2S 算法中最常用的操作。把 3D 点 `(x, y, z, 1)` 乘以变换矩阵：

```cpp
Vec4 TransformPoint(Matrix4x4 M, Vec3 p) {
    Vec4 result;
    result.x = M.m[0][0]*p.x + M.m[0][1]*p.y + M.m[0][2]*p.z + M.m[0][3]*1;
    result.y = M.m[1][0]*p.x + M.m[1][1]*p.y + M.m[1][2]*p.z + M.m[1][3]*1;
    result.z = M.m[2][0]*p.x + M.m[2][1]*p.y + M.m[2][2]*p.z + M.m[2][3]*1;
    result.w = M.m[3][0]*p.x + M.m[3][1]*p.y + M.m[3][2]*p.z + M.m[3][3]*1;
    return result;
}
```

### 变换的复合
多个变换可以通过矩阵乘法合并为一个矩阵：
```
最终矩阵 = 投影矩阵 × 观察矩阵 × 世界矩阵
```
**注意顺序**：变换是从右往左应用的，点先乘世界矩阵，再乘观察矩阵，最后乘投影矩阵。

---

## 4.3 矩阵与3D图形变换关系（09:53）

### 三种基本变换矩阵

#### 1. 平移矩阵
```
┌               ┐
│ 1  0  0  Tx │
│ 0  1  0  Ty │
│ 0  0  1  Tz │
│ 0  0  0  1  │
└               ┘
```
点 (x,y,z,1) 乘以后变为 (x+Tx, y+Ty, z+Tz, 1)。

#### 2. 缩放矩阵
```
┌               ┐
│ Sx 0  0  0 │
│ 0  Sy 0  0 │
│ 0  0  Sz 0 │
│ 0  0  0  1 │
└               ┘
```

#### 3. 旋转矩阵（绕Z轴为例）
```
┌                  ┐
│ cosθ -sinθ 0  0 │
│ sinθ  cosθ 0  0 │
│ 0     0    1  0 │
│ 0     0    0  1 │
└                  ┘
```

### 3D 渲染管线中的坐标空间变换
```
局部坐标(Local)
    │ × 世界矩阵(World)
    ▼
世界坐标(World)  ← 我们从游戏内存读到的敌人坐标就在这里
    │ × 观察矩阵(View)
    ▼
观察坐标(View/Camera)  ← 以摄像机为原点的坐标系
    │ × 投影矩阵(Projection)
    ▼
裁剪坐标(Clip)
    │ ÷ w (透视除法)
    ▼
NDC坐标(-1~1)
    │ × 视口变换
    ▼
屏幕坐标(Screen)  ← 最终在屏幕上的像素位置
```

### 关键理解
- **世界矩阵**：把物体从自身局部坐标放到世界中（位置、旋转、缩放）
- **观察矩阵**：把世界坐标转换到以摄像机为原点的坐标系（相当于"摄像机看到的画面"）
- **投影矩阵**：把 3D 观察坐标投影到 2D 平面，产生近大远小的透视效果

**W2S 算法本质**：跳过世界矩阵（敌人坐标已经是世界坐标），直接执行 `观察矩阵 → 投影矩阵 → 透视除法 → 视口变换` 这四步。

---

## 4.4 观察坐标系与世界坐标系（14:24）

### 世界坐标系（World Space）
- 整个游戏世界的固定坐标系
- 原点在世界中某个固定位置
- 所有物体（玩家、敌人、建筑）的坐标都基于这个坐标系
- **我们从内存中读取的敌人 Position 就是世界坐标**

### 观察坐标系（View/Camera Space）
- 以**摄像机**为原点的坐标系
- 摄像机位置 = 原点 (0,0,0)
- 摄像机朝向 = Z 轴正方向（或负方向，取决于API约定）
- 相当于"把整个世界搬到摄像机面前，让摄像机成为中心"

### 观察矩阵的推导
观察矩阵的作用：把世界坐标中的点，转换到以摄像机为原点的坐标系中。

```
观察矩阵 = 平移矩阵(移到摄像机位置) × 旋转矩阵(转到摄像机朝向)
```

更直观的理解：
1. 把整个世界**平移**，让摄像机位置移到原点
2. 把整个世界**旋转**，让摄像机朝向对准 Z 轴

### 右手坐标系 vs 左手坐标系
| 坐标系 | Z轴方向 | 使用方 |
|--------|---------|--------|
| 右手系 | 摄像机看向 -Z | OpenGL |
| 左手系 | 摄像机看向 +Z | DirectX |

**FPS 游戏中的约定**：
- Unity（左手系）：摄像机 forward 是 +Z
- UE4（左手系）：X 轴是 forward
- 源引擎（右手系）：需要具体分析

### 视角相关参数
- **FOV（Field of View）**：视野角度，影响投影矩阵
- **宽高比（Aspect Ratio）**：屏幕宽/高
- **近裁剪面（Near Plane）**：最近能看到的距离
- **远裁剪面（Far Plane）**：最远能看到的距离

这些参数都编码在**投影矩阵**中。

---

## 4.5 寻找变换矩阵（重要）（25:59）

### 需要找的矩阵
1. **ViewMatrix（观察矩阵）**：4×4，把世界坐标转观察坐标
2. **ProjectionMatrix（投影矩阵）**：4×4，把观察坐标转裁剪坐标
3. 有些游戏直接提供 **ViewProjectionMatrix**（两者已相乘的结果）

### 矩阵在内存中的特征

#### 特征1：数值分布
```
典型 ViewMatrix 数值范围：
┌                              ┐
│  0.8~1.0   0.0      0.0     Tx │  ← Tx,Ty,Tz 通常是较大的负数(摄像机位置取反)
│  0.0       0.8~1.0  0.0     Ty │
│  0.0       0.0      0.8~1.0  Tz │
│  0.0       0.0      0.0     1.0│
└                              ┘
```

#### 特征2：投影矩阵的特殊值
```
典型 ProjectionMatrix（透视投影）：
┌                                  ┐
│ 1.3~2.0   0.0      0.0       0.0 │  ← [0][0] = cot(fov/2) / aspect
│ 0.0       1.3~2.0  0.0       0.0 │  ← [1][1] = cot(fov/2)
│ 0.0       0.0      (远+近)/(远-近)  2*远*近/(远-近) │
│ 0.0       0.0      1.0       0.0 │  ← [3][2] = 1 是透视投影的标志！
└                                  ┘
```
**关键标志**：投影矩阵的 `[3][2]` 位置（行主序）通常是 1.0 或 -1.0，这是透视投影的标志性特征。

### 寻找方法

#### 方法一：特征值扫描
1. 用 CE 的"扫描类型 → 数组"功能
2. 搜索已知的矩阵特征值（如投影矩阵中 `[3][2]=1`）
3. 结合 FOV 变化时矩阵值的变化来验证

#### 方法二：代码回溯
1. 找到绘制相关的函数（如 D3D 的 DrawIndexedPrimitive）
2. 在调用处下断点，查看寄存器/栈中的矩阵指针
3. 游戏引擎通常会把矩阵放在一个全局结构体中

#### 方法三：引擎知识
- **Unity**：`Camera.main.worldToCameraMatrix` 和 `projectionMatrix`
- **UE4**：`FViewInfo` 中的 `ViewMatrices`
- **源引擎**：`CViewRender` 中的 `viewmatrix`

### 验证矩阵是否正确
找到候选矩阵后，用已知点验证：
1. 读取本地玩家世界坐标（已知）
2. 用该矩阵做 W2S 转换
3. 如果转换结果在屏幕中心附近（因为本地玩家就是摄像机），说明矩阵正确

---

## 4.6 矩阵绘制算法详解（重要）（18:05）

### W2S 算法完整流程

这是整个课程的**核心**，把一个 3D 世界坐标点转换为屏幕 2D 坐标。

#### 步骤详解

```
输入：世界坐标点 worldPos (x, y, z)
      观察矩阵 viewMatrix (4×4)
      投影矩阵 projMatrix (4×4)
      屏幕宽度 screenW, 屏幕高度 screenH
输出：屏幕坐标 screenPos (x, y) 或 "在屏幕外"
```

**第一步：世界坐标 → 观察坐标**
```cpp
// 用观察矩阵变换世界坐标
Vec4 viewPos;
viewPos.x = dot(worldPos, viewMatrix.row0) + viewMatrix.m[0][3];
viewPos.y = dot(worldPos, viewMatrix.row1) + viewMatrix.m[1][3];
viewPos.z = dot(worldPos, viewMatrix.row2) + viewMatrix.m[2][3];
viewPos.w = dot(worldPos, viewMatrix.row3) + viewMatrix.m[3][3];
```

**第二步：观察坐标 → 裁剪坐标**
```cpp
// 用投影矩阵变换观察坐标
Vec4 clipPos;
clipPos.x = dot(viewPos, projMatrix.row0) + projMatrix.m[0][3];
clipPos.y = dot(viewPos, projMatrix.row1) + projMatrix.m[1][3];
clipPos.z = dot(viewPos, projMatrix.row2) + projMatrix.m[2][3];
clipPos.w = dot(viewPos, projMatrix.row3) + projMatrix.m[3][3];
```

**第三步：透视除法（Perspective Divide）**
```cpp
// 除以 w，得到 NDC 坐标（范围 -1 到 1）
if (clipPos.w <= 0.0f) {
    // 点在摄像机背后，不在屏幕上
    return false;
}
Vec3 ndc;
ndc.x = clipPos.x / clipPos.w;
ndc.y = clipPos.y / clipPos.w;
ndc.z = clipPos.z / clipPos.w;
```

**第四步：NDC → 屏幕坐标**
```cpp
// NDC 的 x: -1(左) ~ 1(右), y: -1(下) ~ 1(上)
// 屏幕坐标的 x: 0(左) ~ screenW(右), y: 0(上) ~ screenH(下)
screenX = (ndc.x * 0.5f + 0.5f) * screenW;
screenY = (1.0f - (ndc.y * 0.5f + 0.5f)) * screenH;  // Y轴翻转
```

### 合并优化：ViewProjection 矩阵
如果游戏已经提供了 `ViewProjection = Projection × View`，可以一步到位：
```cpp
Vec4 clipPos = TransformPoint(viewProjMatrix, worldPos);
// 然后直接做透视除法和视口变换
```

### 完整 W2S 函数
```cpp
bool WorldToScreen(Vec3 worldPos, Vec2& screenPos, 
                   Matrix4x4 viewProj, int screenW, int screenH) {
    // 1. 矩阵变换
    Vec4 clip;
    clip.x = worldPos.x * viewProj.m[0][0] + worldPos.y * viewProj.m[0][1] + 
             worldPos.z * viewProj.m[0][2] + viewProj.m[0][3];
    clip.y = worldPos.x * viewProj.m[1][0] + worldPos.y * viewProj.m[1][1] + 
             worldPos.z * viewProj.m[1][2] + viewProj.m[1][3];
    clip.z = worldPos.x * viewProj.m[2][0] + worldPos.y * viewProj.m[2][1] + 
             worldPos.z * viewProj.m[2][2] + viewProj.m[2][3];
    clip.w = worldPos.x * viewProj.m[3][0] + worldPos.y * viewProj.m[3][1] + 
             worldPos.z * viewProj.m[3][2] + viewProj.m[3][3];
    
    // 2. 检查是否在摄像机前方
    if (clip.w < 0.1f) return false;
    
    // 3. 透视除法
    float ndcX = clip.x / clip.w;
    float ndcY = clip.y / clip.w;
    
    // 4. 检查是否在屏幕范围内（可选，超出也可以返回用于裁剪）
    // if (ndcX < -1 || ndcX > 1 || ndcY < -1 || ndcY > 1) return false;
    
    // 5. 转屏幕坐标
    screenPos.x = (ndcX * 0.5f + 0.5f) * screenW;
    screenPos.y = (1.0f - (ndcY * 0.5f + 0.5f)) * screenH;
    
    return true;
}
```

### 方框绘制（ESP Box）
有了敌人脚底和头顶的屏幕坐标，就可以画方框：
```cpp
// 敌人世界坐标
Vec3 feetPos = entity.Position;           // 脚底
Vec3 headPos = entity.Position + Vec3(0, 1.7f, 0);  // 头顶(假设身高1.7米)

// 转屏幕坐标
Vec2 feetScreen, headScreen;
WorldToScreen(feetPos, feetScreen, ...);
WorldToScreen(headPos, headScreen, ...);

// 画方框
float boxHeight = feetScreen.y - headScreen.y;
float boxWidth = boxHeight * 0.5f;  // 宽度约为高度的一半
DrawRect(headScreen.x - boxWidth/2, headScreen.y, 
         boxWidth, boxHeight, RED);
```

---

## 4.7 更新函数封装（12:48）

### 为什么需要封装
- 数据读取和绘制分离，代码更清晰
- 每帧只读取一次内存，避免重复读取
- 统一管理数据更新频率，降低 CPU 占用

### 数据结构设计
```cpp
struct EntityData {
    Vec3 Position;
    int Health;
    int Team;
    bool Valid;
};

struct GameData {
    Matrix4x4 ViewMatrix;
    Matrix4x4 ProjMatrix;
    Matrix4x4 ViewProjMatrix;  // 预计算
    Vec3 LocalPlayerPos;
    int LocalTeam;
    EntityData Entities[64];
    int EntityCount;
    int ScreenW, ScreenH;
};
```

### 更新函数
```cpp
void UpdateGameData(GameData& data) {
    // 1. 更新矩阵（每帧都可能变化，因为玩家在移动/转动视角）
    data.ViewMatrix = ReadMatrixFromMemory(VIEW_MATRIX_ADDR);
    data.ProjMatrix = ReadMatrixFromMemory(PROJ_MATRIX_ADDR);
    data.ViewProjMatrix = Multiply(data.ProjMatrix, data.ViewMatrix);
    
    // 2. 更新本地玩家
    data.LocalPlayerPos = ReadVec3(LOCAL_PLAYER_POS_ADDR);
    data.LocalTeam = ReadInt(LOCAL_TEAM_ADDR);
    
    // 3. 更新实体列表
    data.EntityCount = 0;
    for (int i = 0; i < 64; i++) {
        uintptr_t entityPtr = ReadPtr(ENTITY_LIST_ADDR + i * 8);
        if (!entityPtr) continue;
        
        EntityData& ent = data.Entities[data.EntityCount];
        ent.Position = ReadVec3(entityPtr + POS_OFFSET);
        ent.Health = ReadInt(entityPtr + HEALTH_OFFSET);
        ent.Team = ReadInt(entityPtr + TEAM_OFFSET);
        ent.Valid = (ent.Health > 0 && ent.Team != data.LocalTeam);
        
        if (ent.Valid) data.EntityCount++;
    }
}
```

### 线程化更新（可选优化）
```
主线程：负责绘制（60fps）
数据线程：负责读内存（可降频到 30fps，减少内存读取开销）
共享数据：用互斥锁保护 GameData
```

### 性能优化要点
- 矩阵乘法只在矩阵变化时重新计算（ViewProj）
- 实体列表的无效项提前跳过
- 内存读取批量进行（一次读一大块，而不是分散读很多小块）
- 距离过远的实体可以跳过 W2S 计算

---

## 4.8 矩阵绘制代码实现（34:00）

### 完整项目结构
```
ESPProject/
├── main.cpp          // 入口、窗口创建、主循环
├── memory.cpp        // 内存读写函数
├── matrix.cpp        // 矩阵运算、W2S算法
├── render.cpp        // DirectX/OpenGL绘制封装
├── entity.cpp        // 实体数据管理、更新逻辑
└── config.h          // 地址、偏移、常量配置
```

### 主循环完整实现
```cpp
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // 1. 创建 Overlay 窗口
    HWND hwnd = CreateOverlayWindow();
    
    // 2. 初始化绘制设备 (D3D9/11 or OpenGL)
    InitRenderer(hwnd);
    
    // 3. 附加到游戏进程
    HANDLE hProcess = OpenGameProcess("game.exe");
    
    // 4. 主循环
    GameData gameData = {};
    while (true) {
        // 处理窗口消息
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) break;
        }
        
        // 更新数据
        UpdateGameData(gameData, hProcess);
        
        // 开始绘制
        RenderBegin();
        
        // 遍历绘制
        for (int i = 0; i < gameData.EntityCount; i++) {
            EntityData& ent = gameData.Entities[i];
            if (!ent.Valid) continue;
            
            DrawEntityESP(ent, gameData);
        }
        
        // 结束绘制
        RenderEnd();
        
        // 限制帧率
        Sleep(16);  // ~60fps
    }
    
    // 清理
    CleanupRenderer();
    CloseHandle(hProcess);
    DestroyWindow(hwnd);
    return 0;
}
```

### DrawEntityESP 完整实现
```cpp
void DrawEntityESP(EntityData& ent, GameData& data) {
    Vec2 feetScreen, headScreen;
    
    // 脚底坐标转屏幕
    Vec3 feet = ent.Position;
    if (!WorldToScreen(feet, feetScreen, data.ViewProjMatrix, 
                       data.ScreenW, data.ScreenH)) return;
    
    // 头顶坐标转屏幕（身高偏移）
    Vec3 head = ent.Position + Vec3(0, 1.75f, 0);
    if (!WorldToScreen(head, headScreen, data.ViewProjMatrix, 
                       data.ScreenW, data.ScreenH)) return;
    
    // 计算方框尺寸
    float boxH = feetScreen.y - headScreen.y;
    float boxW = boxH * 0.55f;  // 宽高比
    float boxX = headScreen.x - boxW * 0.5f;
    float boxY = headScreen.y;
    
    // 绘制方框
    DrawRect(boxX, boxY, boxW, boxH, Color(255, 0, 0));
    
    // 绘制血条
    float healthRatio = ent.Health / 100.0f;
    DrawRect(boxX - 6, boxY, 4, boxH, Color(80, 80, 80));  // 背景
    DrawRect(boxX - 6, boxY + boxH * (1 - healthRatio), 
             4, boxH * healthRatio, Color(0, 255, 0));  // 血量
    
    // 绘制距离
    float distance = Vec3Distance(ent.Position, data.LocalPlayerPos);
    char distText[32];
    sprintf(distText, "%.1fm", distance);
    DrawText(boxX + boxW + 4, boxY, distText, Color(255, 255, 255));
    
    // 绘制生命值文字
    char hpText[16];
    sprintf(hpText, "%dHP", ent.Health);
    DrawText(boxX, boxY - 16, hpText, Color(255, 255, 0));
}
```

### 常见问题与调试

| 问题 | 可能原因 | 解决方法 |
|------|---------|---------|
| 方框完全不显示 | 矩阵读取错误 / w为负 | 检查矩阵地址、行/列主序 |
| 方框位置偏移 | 坐标偏移不对 / 身高值不对 | 调整 Position 偏移、验证身高 |
| 方框抖动 | 矩阵更新频率不够 / 读内存时序 | 每帧更新矩阵、降低延迟 |
| 近处正常远处消失 | 远裁剪面 / w判断阈值 | 调整 clip.w 阈值 |
| 方框上下颠倒 | Y轴翻转处理错误 | 检查 `1.0f - ndcY` 部分 |
| 方框左右镜像 | 行主序/列主序搞反 | 转置矩阵后重试 |

### 进阶扩展
- **骨骼绘制**：读取骨骼矩阵，对每个骨骼点做 W2S，连线绘制骨架
- **视角弧线（View Angle）**：绘制敌人朝向线
- **可见性检测**：通过射线检测判断敌人是否被墙遮挡
- **距离自适应**：远处的方框变小、文字变小
- **队伍颜色区分**：队友蓝色、敌人红色

---

## 全课程知识体系总结

```
┌─────────────────────────────────────────────────────┐
│                    最终目标：ESP 绘制                  │
├─────────────────────────────────────────────────────┤
│                                                     │
│  内存层 (2.0)          数学层 (4.1~4.4)              │
│  ┌──────────┐         ┌──────────────────┐          │
│  │ 找实体坐标 │         │ 矩阵乘法 / 坐标变换 │          │
│  │ 找变换矩阵 │ ──────→ │ 世界→观察→投影→屏幕  │          │
│  └──────────┘         └──────────────────┘          │
│                             │                       │
│                             ▼                       │
│  框架层 (3.0)          算法层 (4.5~4.6)              │
│  ┌──────────┐         ┌──────────────────┐          │
│  │ Overlay  │         │  W2S 核心算法     │          │
│  │ 绘制循环  │ ──────→ │  方框/血条/距离    │          │
│  └──────────┘         └──────────────────┘          │
│                             │                       │
│                             ▼                       │
│  工程层 (4.7~4.8)                                   │
│  ┌──────────────────────────────────────┐           │
│  │ 数据封装 / 线程更新 / 完整代码落地      │           │
│  └──────────────────────────────────────┘           │
│                                                     │
└─────────────────────────────────────────────────────┘
```

**核心公式一句话**：`屏幕坐标 = 视口变换(透视除法(投影矩阵 × 观察矩阵 × 世界坐标))`
