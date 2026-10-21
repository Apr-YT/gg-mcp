# VERSION_MATCHING.md — 版本族匹配规则（KO 驱动）

> ⚠️ **2026-10-21：本文的「GG 核心版本族」部分已作废。**
> GG 核心注入链路（`lib5_stub` + `libKernelGg.so` + `loader.c`）已整条删除，
> `gg_kernel_load_gg` / `gg_core_versions` 工具不再存在。
> 下文 `6.12.xx`（KO 驱动 vermagic）部分仍然有效，见 `RWPROC33_ADAPTER.md` §十一。

## 核心需求

> 只要是同类型的如 `6.12.xx`，后面 xx 不同都可以加载。

即：**不做精确版本锁定**；同一主版本族（`major.minor` 相同）内，补丁号任意
（`101.1`、`101.2`、`101.3`…）都允许加载。规则在两处生效：

## 0. 实际包分析（宇航员修改器.zip）

对用户提供的包做了解包分析：

| 项 | 值 |
|---|---|
| 包名（伪装） | `com.tencent.gamereva`（防检测，伪装成腾讯手游加速器） |
| versionName | **101.1**（resources.arsc 中） |
| 真实身份 | GameGuardian（FAQ/翻译串证实），社区称"101 版" |
| GG 核心 | arm64 ELF 可执行文件（4.66MB，clang 3.8 编译），APK 内 .so 全部重命名混淆（lib3/lib4/lib6…） |

→ 本仓库 **GG 核心默认版本模式为 `101.*`**；若你有多个 101.x 同族版本，均可加载。

## 1. GG 核心（lib5.so / lib5.so.primary / 混淆名）

- 版本来源：`dumpsys package <pkg>` 的 `versionName=`（agentd 内 popen 解析）。
- 定位：扫描 `/data/user/0/<pkg>/files/` 下**任意子目录**（不限定 GG- 前缀，兼容魔改版），
  核心文件名优先 `lib5.so` / `lib5.so.primary`，兜底选目录内**最大的 AArch64/ARM 可执行 ELF**。
- 匹配：`gg_kernel_load_gg` / `gg_core_versions` 按模式（默认 `101.*`）族匹配。
- 族外（如 `102.0`）：`strict=true`（默认）拒绝并返回 `GGERR_VERSION_MISMATCH`(5)；`false` 告警继续。
- 族内 hook 一次构建全族通用（syscall 270 符号族内稳定），无需按补丁重编译。

## 2. KO 驱动选择

- 版本来源：`.ko` **文件名**中的版本段，如 `driverQX114_6.12.9.ko` → `6.12.9`
  （`version_from_ko_name` 取首个"含点的数字段"，自动跳过名字里的数字如 QX114）。
- 匹配：`ko_load` 扫描 `ko/` 目录，选第一个族匹配的 `.ko` 加载；多个同族 KO 任取其一。
- 族外且 `--ko-strict 1`：拒绝加载，报 `GGERR_VERSION_MISMATCH`。
- 文件名无版本段（如 `driver.ko`）：放行（无法校验），在 `kernel_status.ko_version` 中标空。
- 示例默认模式 `6.12.*`，按你的 KO 驱动实际版本修改 `scripts/start_agentd.sh` 的 `KO_PATTERN`。

## 匹配规则（agentd/version.c）

```
模式 "X.Y.*" 或 "X.Y"   → major==X && minor==Y 即匹配（补丁任意）   ← 默认行为
模式 "X.Y.Z"            → major/minor/patch 全等才匹配
返回值: 1 匹配 / 0 不匹配 / -1 解析失败
```

- 版本串解析容忍前导字母：`"101.1"`、`"6.12.9"`、`"v6.12.9-beta"`、`"6.12"`、`"driverX_6.12.9.ko"`。

## 对照表（示例）

| 对象 | 版本 | 模式 101.* | 模式 6.12.* | 备注 |
|---|---|---|---|---|
| GG 核心 | 101.1 / 101.2 / 101.9 | ✅ | ❌ | 同族任意补丁可加载（实际包 101.1） |
| GG 核心 | 6.12.9 | ❌ | ✅ | 老版本族 |
| KO 文件 | driverQX114_6.12.9.ko | ❌ | ✅ 选它 | 文件名版本提取 |
| 任意 | unknown / 无版本段 | ⚠️ 放行（标注） | ⚠️ | 无法校验 |

## 验证

- `gg_core_versions`：列出 GG 目录 `version / family_match / stub_installed / hook_loaded`。
- `gg_kernel_status`：返回 `ko_module / ko_file / ko_version`（+ `state`），确认实际加载的 KO 与版本。
