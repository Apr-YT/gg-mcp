# GG-MCP 项目记忆（AGENTS.md — 每个会话自动加载的工作区指令）

## 项目概况
Android 悬浮窗内存工具 APK（`apk/` 目录）。**无 Gradle，纯命令行构建**：
```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_apk.ps1 [-Install]   # -Install 同时 adb 安装
```
悬浮窗 UI 设计文档：`docs/FLOAT_WINDOW_UI_DESIGN.md`（GG 101.1 / 宇航员修改器 复刻风格）。

## GG 风格独立 UI Demo（ggtool/ 目录，2026-08 新增）
- **目标**：复刻「宇航员修改器_太空旅客.apk」（`com.tencent.gamereva`）UI 的独立 Demo，第一阶段全 Mock。
- **逆向报告**：`UI_ANALYSIS.md`（APK 信息/UI 类/层级/窗口参数/控件参数；分析产物在 `analysis/`：jadx 反编译、布局 xmltree、rid 解码脚本）。
- **构建**（Android CLI + Gradle，AGP 9.0.1 / Gradle 9.1 / JDK17）：
  ```powershell
  cd ggtool; gradlew.bat assembleDebug   # 中文路径已加 android.overridePathCheck=true
  android install --apks app\build\outputs\apk\debug\app-debug.apk   # Android CLI 安装
  ```
- **包名** `com.ggmcp.tool`；架构：`service/OverlayService`（悬浮球 WindowManager 直挂 + 主面板 Overlay 窗口）+ `ui/{search,saved,freeze,editor,keyboard,region}` + `model/` + `mock/`（DataStore 共享保存/冻结数据）。
- 悬浮球 44dp 可拖动（8dp 判定阈值），位置/透明度存 SharedPreferences；主面板 92%×82% 居中，标题栏可拖动，3 tab（搜索/保存/冻结）。
- 修改值 Dialog 用 `TYPE_APPLICATION_OVERLAY` 窗口（服务上下文无 token），配 `NumericKeyboard` 4×4 内建键盘。
- 实机安装注意：小米设备锁屏/未开「USB 安装」会 `INSTALL_FAILED_USER_RESTRICTED`；需先解锁并在开发者选项开启 USB 安装。

## ⚠️ 铁律：UI/UX 工作必须先查 ui-ux-pro-max 插件（用户已安装并明确要求）
**任何**涉及 UI 结构 / 视觉 / 交互 / 颜色 / 字体 / 间距 / 可用性的改动（改布局、色板、样式、按钮、字号、对话框、通知等）：
1. 先调用 `skill` 工具加载 `ui-ux-pro-max`，按其 pro-rules / quick-reference 审查
2. 用其 search.py 查询：`python .dsh/skills/ui-ux-pro-max/scripts/search.py "<query>" --domain style|color|ux|typography|product`（或 `--design-system`）
3. 插件位置：`.dsh/skills/ui-ux-pro-max/`（SKILL.md + data + scripts + references，Python 3 可运行）
不要凭感觉直接改 UI；查询结果需验证（与产品类型/平台匹配）后再落地，不匹配时明确标注为 fallback。

## 项目 UI 约定（已定稿，勿回退）
- **设计系统**：`design-system/gg-mcp/MASTER.md`（ui-ux-pro-max 插件生成并持久化，Dark OLED + 代码暗色绿强调 + 等宽 + 最小霓虹辉光；GG 101.1 复刻为最终实现，插件推荐作校验基线）
- **主题**：GG 101.1 / 宇航员修改器 复刻 —— 深蓝灰 `#263238` 底 + 白字 + 霓虹值色
- **色板**：`apk/res/values/colors.xml` 语义 token（bg/card/primary/text/addr/hex/green…），全部 UI 必须 `@color/*` 引用，**禁止硬编码色值**
- **悬浮窗**：4 标签（搜索/内存/保存/设置）；标题栏可拖动；右下 `◢` 可缩放；横屏默认 85%×80%
- **对话框**：服务上下文无 Activity token，必须用 `OverlayUi.OverlayDialogBuilder`（`TYPE_APPLICATION_OVERLAY`），普通 `AlertDialog.Builder` 会 BadTokenException
- **对比度红线**：正文 ≥4.5:1；霓虹蓝主按钮 `#55AAFF` 必须配深色文字 `#FF101010`
- 对比度/触控/无障碍标准按 ui-ux-pro-max pro-rules（见文档第十五节）

## 常用命令速查
- 构建安装：`powershell -ExecutionPolicy Bypass -File scripts\build_apk.ps1 -Install`
- 实机验证：`D:\Kiln\ADB\adb.exe`（服务/窗口：`dumpsys window windows | grep ggmcp`；崩溃：`logcat -s AndroidRuntime:E`）
