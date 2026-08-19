# 小猿AI（小猿口算）API 逆向分析报告

> 分析工具：GG-Ghost 套件（gg 内核只读通道，rwproc33/apr_dlkm，零注入零写操作）
> 目标进程：com.fenbi.android.leo (pid 14658) 版本 3.140.1 (31400199)
> 分析日期：2026-08-19
> 数据来源：① 进程内存动态提取（gg 内核直读）② base.apk 静态 dex 字符串全量 ③ native so 静态分析

---

## 1. 执行摘要

小猿AI（小猿口算）是猿辅导系（yuanfudao）教育应用，包名 `com.fenbi.android.leo`。其 API 体系是**典型的大型微服务架构**：

- **传输层**：Chromium Cronet（HTTP/2），非 OkHttp
- **加密层**：`libRequestEncoder.so`（请求加密）+ `libContentEncoder.so`（响应解密），**字符串加密存储、符号全隐藏**，运行时解密
- **安全层**：`libmsaoaidsec.so` / `libmsaoaidauth.so`（鉴权/风控），请求带 `verification` 加密签名 + `enqueueTimestamp` 时间戳防重放
- **服务面**：50+ 微服务前缀（`leo-*`/`solar-*`/`odin-*`），**2955 条 API 路径**静态提取，运行时已实锤多组真实请求

---

## 2. 架构总览（自顶向下）

```
┌─────────────────────────────────────────────────────────────┐
│  Java/Kotlin 业务层（8 个 dex，混淆）                          │
│  服务类：LeoXxxApi / SolarXxxApi / OdinXxxApi                 │
├─────────────────────────────────────────────────────────────┤
│  公共参数层：_productId=611 & platform=android37 & version=...│
│  埋点层：ThinkingData AutoTrack-NETWORK + LeoNetworkListener  │
├─────────────────────────────────────────────────────────────┤
│  加密层：libRequestEncoder.so（请求加密）                      │
│          libContentEncoder.so（响应解密）                     │
│  安全层：libmsaoaidsec.so / libmsaoaidauth.so（签名/风控）     │
├─────────────────────────────────────────────────────────────┤
│  传输层：libcronet.108.0.5359.79.so（HTTP/2, QUIC）           │
│          libhttpdns.so（HTTP DNS）+ libdu.so（私有通道）      │
└─────────────────────────────────────────────────────────────┘
```

**核心链路**：业务层构造路径 → 附加公共参数 → RequestEncoder 加密 → Cronet 经 HTTP/2 发送 → 响应经 ContentEncoder 解密 → 业务层消费。

---

## 3. API 域名体系（8+ 网关，双域 .com/.biz 冗余）

| 域名 | 角色 | 承载服务 |
|---|---|---|
| `xyks.yuanfudao.com` | ⭐ 主 API 网关 | leo-activity / leo-reward / leo-rn-bfe / leo-goblin-mall |
| `xyst.yuanfudao.com` | ⭐ 副 API 网关 | solar-activity / solar-strategy |
| `oapi.yuanfudao.com` | 开放 API 网关 | 通用 OAPI（classes7 配置） |
| `ape-api.yuanfudao.com` | APE 业务 API | ape 系列 |
| `odin.yuanfudao.com` / `odin-leo` / `odin-solar` | 数据上报/分析 | odin-* 统计接口 |
| `klog.yuanfudao.com` | 客户端性能日志 | leo-client-perf/android/leoreport |
| `frog.yuanfudao.com` | 统计上报 | statV2 |
| `dump.yuanfudao.com` | 崩溃 dump | — |
| `m.yuanfudao.com` | IM | im |
| `leo*.fbcontent.cn` | 静态资源/CDN | bh5 H5 包 / leo-cms / leo-gallery / keynote-pdf |
| `solar*.fbcontent.cn` / `*.oss-cn-beijing.aliyuncs.com` | 内容/CDN | 图片/视频/隐私政策 |

**注意**：`xyks` = 小猿口算拼音缩写（XiaoYuan KouSuan），`xyst` = 小猿口算扩展。

---

## 4. 微服务 API 面（2955 条路径，按服务分组标注）

### 4.1 认证与账号（leo-gateway / leo-auth / leo-alchemy-account）

| API 路径 | 方法 | 标注 |
|---|---|---|
| `/leo-gateway/android/auth/sms` | POST | 短信验证码登录 |
| `/leo-gateway/android/auth/password` | POST | 密码登录 |
| `/leo-gateway/android/auth/token` | POST | Token 刷新 |
| `/leo-gateway/android/auth/ali` / `cmcc` / `huawei` / `vivo` / `xiaomi` / `honor` | POST | 运营商/厂商一键登录 |
| `/leo-gateway/android/accounts/switch` | POST | 切换账号 |
| `/leo-gateway/android/accounts/directly/deregister` | POST | 注销账号 |
| `/leo-gateway/android/app/version` | GET | 版本检查 |
| `/leo-gateway/api/speech/convert` | POST | ⭐ 语音转文字（ASR 网关） |
| `/leo-auth/android/user` | — | 用户信息 |
| `/leo-alchemy-account/android/user` | — | 账号详情 |
| `/leo-alchemy-account/android/vip/user/info/v2` | — | ⭐ VIP 会员信息 |
| `/leo-alchemy-account/android/right/click` | — | 权益点击 |
| `/leo-alchemy-order/android/order/log` | — | 订单记录 |

### 4.2 数学/口算核心（leo-math / leo-exam）⭐

| API 路径 | 方法 | 标注 |
|---|---|---|
| `/leo-math/android/math` | — | 数学主入口 |
| `/leo-math/android/exams/v2` / `v3` / `all` / `listen` | GET | 口算考试列表 |
| `/leo-math/android/exams/column/v2` | — | 竖式题 |
| `/leo-math/android/exams/error/exam` | — | 错题考试 |
| `/leo-math/android/exams/knowledgeusage/v2` | — | 知识点练习 |
| `/leo-math/android/exams/exercises/type` | — | 题型练习 |
| `/leo-math/android/exams/delete/v2` | POST | 删除考试 |
| `/leo-math/android/level/game/entrance` | — | 闯关入口 |
| `/leo-math/android/rank` | — | ⭐ 排行榜 |
| `/leo-math/android/recommend/keypoint` | — | 知识点推荐 |
| `/leo-math/android/tab/v6/exercises` | — | 口算专项练习 |
| `/leo-exam/android/paper` / `list` / `search` / `selectors` | GET | ⭐ 试卷查询 |
| `/leo-exam/android/photograph` / `box` / `page` / `page/cut` | POST | ⭐ 拍照搜题/裁剪 |
| `/leo-exam/android/keypoint/catalog` / `knowledge` / `textbook` | — | 知识点/教材目录 |

### 4.3 作业（leo-homework / leo-chinese / leo-english）

| API 路径 | 标注 |
|---|---|
| `/leo-homework/android/homework/detail` / `merged` / `question` / `result` | 作业详情/题目/结果 |
| `/leo-homework/android/homework/submit/oral` / `chinese` / `read` / `listen` / `poetry` / `unit` / `common` / `knowledge` / `column` / `amusing` | ⭐ 作业提交（按题型分 10+ 接口） |
| `/leo-homework/android/homeworks/preview/v2` / `publish/v2` / `submit` / `time` | 作业预览/发布/提交/时长 |
| `/leo-chinese/android/exercises` / `home` / `listen` / `history/listen` / `customize/pinyin` / `word` | 语文练习（听写/拼音/生字） |
| `/leo-english/android/exercises` 系列 | 英语练习 |

### 4.4 口语/背诵/语音评分（leo-apeman-*）⭐ 核心 AI 能力

| API 路径 | 标注 |
|---|---|
| `/leo-apeman-parrot-scorer/speaking` | ⭐ 英语口语评分（parrot=鹦鹉） |
| `/leo-apeman-recite-scorer/recite` | ⭐ 背诵评分 |
| `/apeman-cn-voice-input/asr` | 中文语音输入 ASR |
| `/leo-gateway/api/speech/convert-to-text-stream-v2` | 语音流式转写 |

### 4.5 AI 对话/答题（leo-ai-chat / leo/aichat）

| API 路径 | 标注 |
|---|---|
| `/leo-ai-chat/android/ai` | ⭐ AI 对话主接口 |
| `/leo-ai-chat/android/image` | AI 图像理解 |
| `/leo-ai-chat/android/learning` | AI 学习助手 |
| `/leo/aichat/socket/connect` / `close` / `open` / `end` / `error` | ⭐ AI 聊天 **WebSocket** 通道 |
| `/leo/ai/answer/camera` | AI 拍照答题 |
| `/leo/camera/aggregation/ai/recognize` | ⭐ 拍照识别（聚合 AI） |

### 4.6 活动/商城/激励（leo-activity / leo-reward / leo-goblin-mall / solar-activity）

| API 路径 | 方法 | 标注 |
|---|---|---|
| `/leo-activity/android/pet/cookie-balance` | GET | ⭐ 宠物饼干余额（运行时实锤 [200]） |
| `/leo-activity/android/pet/card` / `exercise` / `cookie` | — | 宠物养成 |
| `/leo-activity/android/statistics/submit` | POST | 活动统计提交 |
| `/leo-reward/android/task/hp` | GET | ⭐ 任务体力（运行时实锤，带 xiaoxueId 参数） |
| `/leo-goblin-mall/android/goods/click-read/display` | GET | 商城商品展示（运行时实锤） |
| `/solar-activity/android/activity/6` | GET | 活动 6（运行时实锤，带 leoExerciseGrade 等） |
| `/solar-activity/android/strategy/sku/multi` | GET | ⭐ SKU 批量查询（商城定价，运行时实锤） |
| `/leo-alchemy-order/android/order/log` | — | 订单日志 |

### 4.7 内容/工具（leo-pdf / leo-poetry / leo-composition / leo-cn-dictionary / leo-error-book）

| API 路径 | 标注 |
|---|---|
| `/leo-error-book/android/*` | ⭐ 错题本 |
| `/leo-composition/android/*` | 作文 |
| `/leo-poetry/android/*` | 古诗 |
| `/leo-pdf/*` | PDF 打印 |
| `/leo-cn-dictionary/*` | 汉语词典 |
| `/leo-feedback/android/*` | 用户反馈 |
| `/leo-star/*` | 星星奖励 |

### 4.8 日志/埋点（klog / frog / odin）

| API 路径 | 方法 | 标注 |
|---|---|---|
| `/leo-client-perf/android/leoreport` | POST | ⭐ 客户端性能上报（运行时实锤 [200]，klog 域名） |
| `/frog.yuanfudao.com/statV2` | POST | 行为统计 |
| `/odin-leo.yuanfudao.com` 系列 | POST | 数据上报 |

---

## 5. 公共参数体系（每个请求自动附加）⭐

运行时从真实请求 `GET /leo-reward/android/task/hp` 抓取：

```
_productId=611            # 产品 ID（小猿口算）
platform=android37        # 平台标识（android + API 级）
version=3.140.1           # 版本号
vendor=xiao_mi            # 渠道厂商
deviceCategory=phone      # 设备类型
av=5                      # 应用版本代
webviewVersion=146        # WebView 版本
whRatio=2.17              # 宽高比（UI 适配）
isBackground=0            # 前后台
xiaoxueId=0               # 小学阶段 ID（业务参数）
```

另附请求上下文（埋点字段）：`pageFrogSessionId`（页面会话）、`frogSource=leo`、`VIPType=0`、`common_role`、`common_grade`、`payuser`、`exerciseGrade`、`trace_id`（链路追踪）。

---

## 6. 加密与安全机制 ⭐

### 6.1 传输协议
- **HTTP/2**（`protocolName: HTTP_2`，Cronet 确认），HTTPS + TLS
- `okhttpCacheSource: miss_or_bypass`（缓存旁路，动态数据不缓存）

### 6.2 请求签名（动态实锤）
```
body=phone=***&verification=hOnJ7P0HLEN4X5bIDub3ZBYOV9uhy1d3rsEe4RZEsXW6UCSL%2BwM4pnuQZ%2F6Kz6578amjiahHF31nI40gr1Kkr9UG3UfHy6xGzJFJ%2F2F9TiDeoW38HksB5BoC2jYWeNkKw1beiPlm4avcBIct%2Fs78QoOybzpP...
```
- `verification` 参数 = **加密签名**（Base64 字符集 + `%2B`/`%2F` 转义 → 标准 Base64 URL 编码），由 libmsaoaidsec/libRequestEncoder 生成
- `enqueueTimestamp` = 请求发起时间戳（**防重放**）
- 短信验证码接口的 `phone` 参数以 `***` 打码（埋点脱敏，非实际传输）

### 6.3 加密库特征（静态实锤）
- `libRequestEncoder.so`（919KB）：**字符串整体加密**（.rodata 为乱码短串 `v-,O`/`.d2w` 等），符号全隐藏（dynsym 仅导入），JNI 方法名仅残留 `getEncodedP` 片段
- `libContentEncoder.so`（298KB）：同上模式，负责响应解密
- `libmsaoaidsec.so`（694KB）/ `libmsaoaidauth.so`（418KB）：安全组件（签名/风控），JNI_OnLoad 导出 3 处
- `libdu.so`（1.2MB）：私有网络通道库（socket/recvfrom/sendto，UDP 特征）

### 6.4 埋点
- ThinkingData AutoTrack（`AutoTrack-NETWORK: /path::GET[200]` 格式）
- App 自研 `LeoNetworkEventListener`（callStart/callEnd 时序统计：ConnectDuration/SecureConnectDuration/TotalFromRequestStartDuration）

---

## 7. 运行时实锤请求清单（gg 内存提取，可信度最高）

| 时间戳 | 方法 | 路径 | 状态 | 域名 |
|---|---|---|---|---|
| 1787117673 | GET | /leo-activity/android/pet/cookie-balance | 200 | xyks |
| 1787117673 | GET | /leo-reward/android/task/hp | 200 | xyks |
| 1787117959 | GET | /solar-activity/android/activity/6 | 200 | xyst |
| 1787117959 | GET | /solar-activity/android/strategy/sku/multi | 200 | xyst |
| 1787117673 | POST | /leo-client-perf/android/leoreport | 200 | klog |
| — | GET | /leo-goblin-mall/android/goods/click-read/display | — | xyks |
| — | GET | /leo-rn-bfe/api/zip/list | — | xyks |

---

## 8. 关键结论与检测面

### 8.1 结论
1. 小猿AI API = **加密微服务网关架构**，与腾讯 ACE 系（CF/三角洲）完全不同——**无内核反作弊**，纯用户态加密 + 服务端风控
2. 静态分析被**字符串加密 + 符号隐藏**双保险挡住，但**运行时内存即明文**（gg 内核直读天然穿透，与 CF metadata 同理）
3. 2955 条 API 路径中，**作业提交/拍照识别/AI 对话/口语评分**是核心高价值接口
4. 接口命名规范统一（`服务/平台/模块/动作`），逆向成本低

### 8.2 检测面（防御方手段）
| 手段 | 位置 | 规避难度 |
|---|---|---|
| 请求签名 verification | libmsaoaidsec | 高（需复刻签名算法） |
| 时间戳防重放 enqueueTimestamp | 服务端 | 中 |
| 字符串加密 | libRequestEncoder/.rodata | 低（运行时明文） |
| 符号隐藏 | 全部 so | 低（动态分析可绕） |
| 证书校验/TLS | Cronet | 中（需注入 CA 或 hook） |
| 行为埋点 AutoTrack | ThinkingData | 中（服务端行为分析） |

### 8.3 安全提示
本报告仅用于授权安全研究与合规测试。接口调用需遵守小猿口算服务条款，禁止用于绕过计费、刷题作弊、数据爬取等违规用途。

---

## 9. 附：产物文件

| 文件 | 说明 |
|---|---|
| `api_paths_raw.txt` | 2955 条 API 路径全量清单 |
| `dex_api_strings.txt` | dex 全量 API 相关字符串（19848 条） |
| `urls.txt` / `urls_api.txt` | 内存提取 URL |
| `all_domains.txt` | 全量域名清单 |
| `so/*.so` | 7 个 API 相关 native 库样本 |
