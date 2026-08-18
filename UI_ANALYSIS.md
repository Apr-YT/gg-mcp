# UI 逆向分析报告 — 宇航员修改器︎_太空旅客.apk

> 分析对象：`C:\Users\AprYT\Downloads\宇航员修改器\宇航员修改器︎_太空旅客.apk`（14.8 MB）
> 分析方法：7-Zip 解包 → aapt2 dump（Manifest / resources / AXML）→ jadx 反编译 classes.dex（605 类，含 `--show-bad-code` 二轮）→ 自写 NPFog 资源 ID 解码器还原全部混淆 ID → 逐布局 dump 二进制 XML。
> 分析产物目录：`analysis/`（astronaut_ref.apk 副本、jadx_out/、jadx_bad/、layouts/*.xmltree、rid_map.json、rid_decode.txt、decode_rids.py、map_layouts.py、dump_layouts.py）。
> 日期：2026-08-17

---

## 1. APK 基本信息

| 项 | 值 | 来源 |
|---|---|---|
| package | `com.tencent.gamereva` | aapt2 badging |
| versionName | `太空旅客` | Manifest |
| versionCode | `27904` | Manifest |
| minSdk / targetSdk / compileSdk | `10` / `22` / `29` | Manifest |
| application-label | `宇航员修改器︎` | Manifest |
| isGame | true | Manifest |
| Launcher Activity | `com.tencent.gamereva.MainActivity`（extends `android.ext.ar`） | Manifest + DEX |
| 其他组件 | `Receiver`、`AnalyticsService`（service）、`FileProvider`、`Instrumentation` | Manifest |

**组件混淆说明**：Manifest 与 DEX 中组件名均为无包名的短类名（`Application`、`MainActivity`、`Receiver`、`AnalyticsService`、`Instrumentation`、`FileProvider`），实际都位于 `com.tencent.gamereva` 包且继承 `android.ext.*` 混淆基类；UI 类全部伪装在 `android.*` 命名空间下（`android.ext.*`、`android.fix.*`、`android.a~c.*`、`android.lang.*`），用于对抗静态分析。字符串经 `obfuse3.obfuse.StringPool` 混淆，资源 ID 经 `np.NPFog.d()`（XOR 链）混淆，资源文件名/资源名被替换为阿拉伯字符与水印"神诺制作 倒卖必追"。

**权限**（Manifest 声明）：`SYSTEM_ALERT_WINDOW`（悬浮窗核心）、`READ/WRITE_CLIPBOARD`、`READ/WRITE_EXTERNAL_STORAGE`、`WRITE_MEDIA_STORAGE`、`VIBRATE`、`INTERNET`、`ACCESS_NETWORK_STATE`、`GET_TASKS/REAL_GET_TASKS/GET_DETAILED_TASKS`、`INTERACT_ACROSS_USERS(_FULL)`、`READ_LOGS`、`UPDATE_APP_OPS_STATS`、`CHANGE_COMPONENT_ENABLED_STATE`、`REQUEST_INSTALL_PACKAGES`、`REQUEST_DELETE_PACKAGES`、自定义 `ACCESS_SUPERUSER`（root 探测）。

**UI 技术栈**：Android Views / XML（无 Compose），全部自定义 `android.fix.*` View 子类（LinearLayout/TextView/Button/ListView/ScrollView/ImageView/ImageButton/TabHost/FrameLayout/CheckBox/ProgressBar/SeekBar…），布局以 XML 定义但根元素/控件类替换为 `android.fix.*`；核心 UI 由 `android.ext.MainService`（悬浮窗宿主）动态构建。

---

## 2. UI 关键类

### 2.1 `android.ext.MainService`（核心宿主，extends `rh`，1986 行）
- **作用**：整个悬浮窗系统的中枢：持有悬浮球 `HotPoint l`、主面板 Dialog `me p`、TabHost `o`、各 ListView（搜索列表 `I`、保存列表 `J`、内存列表 `S`、地址列表 `D`）、适配器（`SavedListAdapter an`、`ConfigListAdapter aq`）、脚本引擎 `Script d`、定时器 `TimersEditor ae/ag`。
- **UI 构建**：`t()` 方法 inflate `R.layout.service_dialog`（TabHost 根），绑定全部控件，注册 4 个 tab：配置 `config_page`、搜索 `search_page`、保存 `saved_page`、内存编辑器 `memory_editor_page`；默认 `setCurrentTab(1)`（搜索页）。Tab 图标用 `R.id.*_tab_icon` ImageView，tab 计数器用 `*_tab_counter` TextView。
- **悬浮球/面板显隐**：`d()` 显示悬浮球 `HotPoint`；`c(true)`/`g()` 关闭主面板并回退到悬浮球；`f()` 创建并显示主面板 Dialog `me`；`V()` 根据配置决定面板形态（`Config.B` 标志位，如 4=自动暂停、128=从游戏隐藏）。
- **与系统交互**：前台服务通知（`Notification.Builder`，可加"截图"Action）、`WindowManager`、ActivityManager（进程列表 `z j`）、SharedPreferences（位置/透明度持久化）。
- **常量**：`af[176]`/`ah[76]` 布尔数组是 timer 配置位图；`q[3][]` 是 pj 工具栏按钮矩阵（每 tab 一栏 WrapLayout `B[0..2]`）。

### 2.2 `android.ext.FloatPanel`（抽象基类，extends `android.fix.LinearLayout`，412 行）
- **作用**：所有可拖动悬浮面板（悬浮球 HotPoint 等）的基类，封装 WindowManager 窗口参数、拖动、位置持久化。
- **窗口参数**：见 §4。
- **拖动逻辑**：`dispatchTouchEvent` 捕获 rawX/rawY；移动超过 `n = 36dp`（`Tools.a(36f)`）判定为拖动 → 记录按下点、更新 LayoutParams.x/y 并 `motionEvent.setAction(3)` 取消子控件点击；抬起时若未拖动则视为点击（`o` 标志）。
- **位置持久化**：按方向（横/竖）分别存 `pref + getPrefName() + orientation` 前缀的 x/y；`j()` 写回 SharedPreferences。
- **方法**：`e()` 添加到 WindowManager、`g()` 移除、`d()/f()/h()` 异步切换、`setLayoutAlpha()` 透明度（默认 1.0f）。
- 构造中含反射调用（`Context.getMethod("getSystemService"...` 等）为反检测混淆。

### 2.3 `android.ext.HotPoint`（悬浮球，extends FloatPanel，337 行）
- **作用**：GG 风格的悬浮球（速度修改指示 + 变速控制入口），`getInstance()` 单例，inflate `R.layout.hot_point_view`。
- **布局**（hot_point_view）：`speed_dec`（−按钮，48dp，`@drawable` 减速图标）→ `HotFrame`（id `hot_frame`，内含两个进度条 `progress_bar`/`progress_bar_stage` + 百分比文本 `progress_bar_text`）→ `speed_inc`（+按钮，48dp）；底部 `speed_text` 显示当前速度值（如 `9 000 000 000`）。`HotFrame` 内还有两个 ImageView（`hot_point_icon` 游戏图标 + 一个动画 drawable）。
- **窗口参数**：`j()` 单独构造：gravity=51、type=`rv.a()`、format=-2、flags=`rv.c()|512`（NOT_FOCUSABLE|FLAG_NOT_TOUCH_MODAL? 512=FLAG_LAYOUT_IN_SCREEN 见 §4）、alpha=1.0f、wrap 尺寸。
- **交互**：点击（`OnClickListener`）切换主面板；长按（`OnLongClickListener`）拖入游戏/唤起菜单；`a(byte,long,long,int,int,String)` 更新进度条与文本（speedhack 进度显示）。

### 2.4 `android.ext.Searcher`（搜索面板控制器，732 行）
- **作用**：搜索 tab 的逻辑控制器。inflate `R.layout.service_searcher`；实现 `OnClickListener/TextWatcher/OnFocusChangeListener/OnCheckedChangeListener/OnItemSelectedListener`。
- **模式**：构造参数 `i` 区分 5 种搜索模式（`a[5]` 类型数组）：0=普通数值、1=??、2=差分(Difference)、3=地址(Mask)、4=地址范围；`F[5][11]` 可见性矩阵控制 11 个区块显示。
- **控件**（service_searcher 布局，见 §5.2）：`SystemSpinner h`（符号选择器 ≠/=）、`EditTextMulti j`（值输入，weight=1）、`HexConverter`（48dp）、`EditText edit_mask`（掩码）、`SystemSpinnerType u`（类型：Byte/Word/Dword/Qword/Float/Double/Xor）、`CheckBox mode_hacking`/`ordered`、`Button hex1/hex2/min_group_size`（值格式快捷：`::57`、`HEX+U8`、`HEX+U16`）、模糊搜索按钮行（`equal_btn/not_equal_btn/larger_btn/smaller_btn`）、`EditText limit`（搜索上限）、`MemoryRange memory_range`（区域选择）、`Button cancel`/`ext_btn`（确认搜索）。
- **与系统交互**：`MemoryRange d.setType(a[i])` 同步类型；点击 hex 按钮改写 EditText 内容（16 进制/字符串转换）；`D` 按钮绑定外部监听（搜索执行）。

### 2.5 `android.ext.RegionList`（区域列表，extends ImageButtonView，329 行）
- **作用**：内存区域数据源（静态），二分查找地址所属区域；区域条目 `qs`（含 start/end/permissions/name/path）；`a(long)` 返回区域名（如 `libxxx.so`）；权限解析 `a("rwxs")` → 标志位。
- **UI 角色**：作为"范围"下拉的入口按钮（extends `ImageButtonView`），点击展开区域选择对话框（`R.layout.memory_range`）。

### 2.6 `android.ext.MemoryRange`（内存范围选择，extends LinearLayout，197 行）
- **作用**：搜索范围 UI（inflate `R.layout.memory_range`），3 态按钮循环：`全部内存(all_memory)` → `仅此区域(only_memory)` → `附近(nearby)`。
- **控件**：Button `c`（模式切换）、EditText `e/f`（起止地址，hex）、EditText `h`（中心地址）、CheckBox `i/j`（±范围）、EditText `k`（范围大小，hex）、`InternalKeyboard.a(e, 1)` 挂接内建键盘。

### 2.7 `android.ext.SavedListAdapter`（保存列表适配器，extends BaseAdapterIndexer，367 行）
- **作用**：保存列表 ListView 适配器，数据容器 `android.c.e`，条目模型 `qx`（地址 b / 值 c / 类型 d / 冻结标志 f）。
- **UI**：`getView` inflate `R.layout.service_saved_item`（见 §5.4）；支持紧凑/完整两种布局（`Config.q` 位与 `config_use_small_list_items`）；条目含 CheckBox（选择）、冻结按钮（`freeze`，切换 `qx.f` 并刷新图标）、地址/值/类型/区域 TextView、删除按钮（`addr_item_remove`，弹确认框）。
- **冻结机制**：通过 `MainService.k`（`ex` 冻结引擎）注册/注销冻结项。

### 2.8 `android.ext.ItemContextMenu`（长按菜单，275 行）
- **作用**：列表项长按菜单（`OnItemLongClickListener`），弹 AlertDialog（`i.a(...).setAdapter(...)` 自绘 ListAdapter `kv`），条目 = 图标 + 文本。
- **菜单项**（动态生成）：偏移计算器 `offset_calculator`、指针搜索帮助 `help_pointer_search_title`、附近搜索 `search_nearby`、跳转地址 `goto_address + 地址`、跳转指针 `goto_pointer`、分配页 `allocate_page`（若页不在区域表）、复制地址 `copy_address`、复制值/复制十六进制/复制原始十六进制 `copy_value/copy_hex_value/copy_rhex_value`、复制操作码 `copy_opcode`（ARM/Thumb 反汇编结果）、修改地址类型 `change_address_type`（仅保存列表页）、底部"取消"。
- 图标均用 `R.drawable.ic_*_white_24dp`（Material 风格白色图标）。

### 2.9 `android.ext.InternalKeyboard`（内建键盘，extends `android.fix.g`[View]，431 行）
- **作用**：GG 内建数字/十六进制键盘，替代系统输入法。基于 `android.inputmethodservice.Keyboard` + XML 键盘定义（`R.xml.keyboard` 与 `R.xml.empty`，混淆为 `ۤۧۨ.xml` 等 Keyboard 根文件）。
- **模式**：`a(useInternal, fn, small, hex)` 组合 → `k_num/k_hex/k_fn/k_small/k_ext` 5 套键盘定义；`R.dimen.key_width × 每行键数`、`key_height × 行数` 动态计算自身尺寸。
- **按键码**：<5000 直接输出字符；5000=DEL(67)、5001=?、5002=`ps.a`、5003=`ps.b`、5004/5005=左/右移动光标、5006=Fn 切换、5007=剪贴板粘贴、5008=全选、5009=小键盘切换、5010=?。
- **集成**：`KeyboardLayout`（`R.layout.keyboard`：ScrollView `kbd_scroll`/FrameLayout `kbd_frame`/InternalKeyboard `keyboard`）包装目标 View，`InternalKeyboard.a(view, bool)` 静态入口；监听窗口焦点 EditText 并注入按键（`dispatchKeyEvent`）。

### 2.10 `android.ext.KeyboardLayout`（键盘容器，87 行）
- extends LinearLayout；`a()` 按横竖屏调整方向与尺寸（竖屏：orientation=1，ScrollView 高 -1；横屏：orientation=0，FrameLayout 高 -2）。

### 2.11 `android.ext.TimersEditor`（定时器/随机数编辑器，257 行）
- **作用**：速度修改的定时器表（`timers` 44 行 / `rand` 19 行），每行 4 个 CheckBox（app/java/system/other）+ 行号 TextView；Header 行可点击排序。ListView（`R.layout.timer_table`）承载 `timer_header`/`timer_row`，行背景色区分启用/禁用（-16755456 红 / -11206656 蓝），`FastScrollerFix` 快速滚动。

### 2.12 `android.ext.SystemSpinner` / `SystemSpinnerType`（下拉选择，93/61 行）
- SystemSpinner extends Button：点击弹 `AlertDialog.setSingleChoiceItems`（单选列表），数据源 `android.fix.j`（SparseArray 包装）；`setSelected` 更新文本并回调。
- SystemSpinnerType 扩展：类型选择（BYTE/WORD/DWORD/QWORD/FLOAT/DOUBLE/XOR），条目颜色按类型着色（`d.k(i)`）。

### 2.13 `android.ext.ShowApp`（进程选择，13818 字节）与 `android.ext.ConfigListAdapter`（配置列表，44200 字节）
- ShowApp：`R.layout.show_app` 应用/进程选择（含 app/java/system/other 过滤）。
- ConfigListAdapter：配置页 ListView 适配器（`R.layout.service_config`），内含大量配置项（见 §5.5），支持 seekbar、单选、多选、文本输入、开关等控件类型。

### 2.14 `com.tencent.gamereva.MainActivity` / `android.ext.ar`（启动入口）
- MainActivity extends `ar`（extends `android.fix.a`[Activity]）。`ar.onCreate`：显示 OnlineDialog、`setContentView(R.layout.main)`（安装器/启动器界面：loader/installer/installing/progress_bar/starter/btn_start_usage/btn_stop_service/check/fix_it/language/help），然后启动 `MainService`（悬浮窗服务）。
- `ActivityMain`（另一个 Activity 入口，`ar.b()` 按是否 root 二选一）用于非 root 模式。
- `AnalyticsService` extends `android.app.Service`（空实现占位）；`Application` extends `android.ext.ae`（初始化 StringPool/日志）。

---

## 3. UI 层级（还原）

### 3.1 悬浮球窗口（WindowManager 直接 addView）
```
WindowManager (TYPE_APPLICATION_OVERLAY / TYPE_PHONE)
└── HotPoint (android.ext.HotPoint = FloatPanel, hot_point_view)
    └── LinearLayout (horizontal, center)
        ├── ImageButtonView speed_dec (48dp, 减速)
        ├── HotFrame (hot_frame)
        │   ├── ImageView hot_point_icon (48dp 游戏图标, 默认 GONE)
        │   ├── ImageView 0x7f0b0159 (48dp 动画 drawable)
        │   └── LinearLayout progress_layout (weight)
        │       ├── TextView progress_bar_text (10dp, "30.9%", 背景 #80000000)
        │       ├── ProgressBar progress_bar (max=100, progress=30)
        │       └── ProgressBar progress_bar_stage (max=100, progress=60)
        ├── ImageButtonView speed_inc (48dp, 加速)
        └── TextView speed_text ("9 000 000 000", 背景 @drawable)
```

### 3.2 主面板（Dialog `me`，内容 = service_dialog）
```
Dialog (me, 无标题栏, 透明背景, 不可外部取消, 不可 BACK 取消)
└── TabHost (android.fix.TabHost, 背景 #c0000000, 全屏)
    └── LinearLayout (vertical)
        ├── LinearLayout tabs (id=tabs, horizontal, 高度 wrap)
        │   ├── FrameLayout 48dp
        │   │   ├── ImageButtonView app_icon (48dp, src=ic_gg)
        │   │   └── ImageView small_icon (20dp)
        │   ├── TabWidget (weight=1, 高 wrap)
        │   │   ├── FrameLayout config_tab (minH 48dp) → ImageView config_tab_icon (48dp, padding 12dp)
        │   │   ├── FrameLayout search_tab  → ImageView search_tab_icon + TextView search_tab_counter ("21", 背景 counter)
        │   │   ├── FrameLayout saved_tab   → ImageView saved_tab_icon + TextView saved_tab_counter ("74")
        │   │   └── FrameLayout memory_tab  → ImageView memory_tab_icon (48dp)
        │   ├── ImageButtonView menu_btn_land (48dp, 菜单)
        │   └── ImageButtonView back_btn (48dp, 关闭)
        ├── LinearLayout info_row (horizontal, 高 wrap, weight=0)
        │   ├── ImageButton process_pause (32×26dp, 暂停)
        │   ├── TextButtonView app_name (weight=10, 应用名)
        │   ├── LinearLayout info_mem (weight=10)
        │   │   ├── TextButtonView filter (weight=10, 过滤)
        │   │   └── TextButtonView value_format ("h,D,F")
        │   ├── LinearLayout info_cnt
        │   │   ├── ProgressBar count_wait (26dp, progressBarStyleSmall)
        │   │   └── TextButtonView found_count ("(0)")
        │   ├── TextButtonView saved_format ("h,D,F")
        │   ├── ImageButton menu_btn (32×26dp)
        │   └── ImageButton list_refresh (32×26dp)
        ├── View (1dp 分隔线 #c0ffffff)
        ├── LinearLayout toolbar_row (horizontal)
        │   ├── WrapLayout search_toolbar (48dp, weight=1, 动态按钮)
        │   ├── WrapLayout saved_toolbar (GONE, weight=1)
        │   ├── WrapLayout memory_toolbar (GONE, weight=1)
        │   └── ImageButtonView more_btn (45×48dp, 更多)
        ├── View (1dp 分隔线 #c0ffffff)
        ├── FrameLayout (id=@android:id/tabcontent, weight=1) ← TabHost 内容区
        │   ├── LinearLayout search_page (id=search_page)
        │   │   ├── ListView addr_list (GONE 默认)     ← 搜索结果列表
        │   │   └── ScrollView nothing_found_view (weight=1)
        │   │       └── TextView nothing_found_text    ← 空态提示
        │   ├── LinearLayout memory_editor_page (id=memory_editor_page)
        │   │   └── ListView mem_listview (fastScroll)
        │   ├── LinearLayout config_page (背景 #ff000000)
        │   │   └── ListView config_list
        │   └── LinearLayout saved_page (id=saved_page)
        │       ├── ListView saved_list
        │       └── ScrollView empty_list_view (weight=1)
        │           └── TextView empty_list_text
        └── TextButtonView status_bar ("8.40.0 + Ch,Jh,Ca,Cd,Cb,A,S,O", marginRight 60dp)
```
> 搜索页 `search_page` 中实际嵌入 `Searcher` 面板（`R.layout.service_searcher`）与结果列表 `addr_list`；`addr_list` 默认 GONE，搜索完成后显示。tab 内容视图在 `t()` 中被重新 addView 到 tabcontent（隐藏原 TabHost 自动生成的容器）。

---

## 4. WindowManager 参数（已确认值）

| 参数 | 值 | 来源 |
|---|---|---|
| type | `2038 (TYPE_APPLICATION_OVERLAY)` SDK≥26；否则 `2002 (TYPE_PHONE)` | `rv.a()`（主面板/悬浮球）；`rv.b()` 2003/2038 供键盘等 |
| flags | 默认 `8 (FLAG_NOT_FOCUSABLE)`；`rv.c()` = `Config.u` 可配置 | `rv.c()` |
| flags 追加 | `\| 256 (FLAG_LAYOUT_IN_SCREEN)`（FloatPanel 构造）| FloatPanel 构造 |
| flags 追加 | `\| 512 (FLAG_NOT_TOUCH_MODAL)`（HotPoint 单独）| HotPoint.j() |
| format | `-2 (PixelFormat.TRANSLUCENT)` | FloatPanel/HotPoint |
| gravity | `51 (Gravity.TOP \| START)` | FloatPanel/HotPoint |
| width / height | 初始 `-2`；FloatPanel 构造后 `40dp`（`Tools.a(40f)`）；HotPoint wrap | 构造 |
| alpha | 初始 0 → `a()?1.0f: SharedPreferences 默认 1.0f`（可设置透明度）| FloatPanel |
| x / y | 从 SharedPreferences 读取（按方向分键），拖动更新 | FloatPanel |
| 拖动阈值 | `36dp`（`n = Tools.a(36f)`） | FloatPanel |
| 尺寸常量 | 悬浮球相关 48dp 触控区；主面板全屏 | 布局 |

**主面板承载方式（重点）**：主面板不是 WindowManager.addView，而是 **Dialog**（`me extends Dialog`，`setContentView(Tools.d(view))`、`requestWindowFeature(1)` 无标题、`setBackgroundDrawable(new ColorDrawable(0))` 透明、`setCanceledOnTouchOutside(false)`、`setCancelable(false)`）。Dialog 窗口由系统按 theme 创建；`b()` 里可加 `FLAG_LAYOUT_IN_SCREEN`(16777216) 等。这解释了为何无需手动管理主面板 LayoutParams —— 悬浮球（HotPoint）才是 WindowManager 直挂窗口。

---

## 5. 关键控件参数（来自布局 XML 实测值）

### 5.1 service_dialog（主窗口）
- 根背景 `#c0000000`（半透明黑）；全屏 `-1/-1`。
- Tab 图标：48dp ImageView + padding 12dp；tab 容器 minHeight 48dp。
- 计数器 TextView：`textAppearance @style(0x7f090002)`、白字、背景 `@drawable/counter`。
- info_row：process_pause/menu_btn/list_refresh 32×26dp；app_name/filter 为 TextButtonView 文本按钮；found_count 显示 `(0)`。
- 工具栏 WrapLayout 高 48dp；more_btn 45×48dp padding 12dp。
- 分隔线：1dp `#c0ffffff`。
- status_bar：文本 `"8.40.0  +  Ch,Jh,Ca,Cd,Cb,A,S,O"`，marginRight 60dp（避开折叠屏挖孔）。

### 5.2 service_searcher（搜索面板）
- 根 ScrollView，paddingLeft/Right 6dp。
- 值行：`SystemSpinner sign_spinner`（48dp，默认 `≠`）→ `EditTextMulti number`（weight=1，imeOptions actionDone）→ `HexConverter`（48×48dp）。
- 掩码行：`EditText edit_mask`（weight 填满）+ `TextView mask_view`。
- 偏移行：`EditText offset`（inputType number，imeOptions actionNext）+ CheckBox `hex`（checked=true）。
- 类型行：TextView `type` + `SystemSpinnerType`（weight 填满，文本大小 button_text_size）。
- 模式行：CheckBox `mode_hacking`（weight=1, 高48dp）+ CheckBox `ordered`（GONE）+ Button `min_group_size`（`::57`，GONE，48dp）+ Button `hex1`（`HEX+U8`，GONE）+ Button `hex2`（`HEX+U16`，GONE）。
- 模糊行：TextView `fuzzy_text` + 2 行按钮：`equal_btn/not_equal_btn`（weight=1）、`larger_btn/smaller_btn`（weight=1）。
- 限制行：TextView `limit` 标签 + `EditText limit`（weight 填满）。
- `MemoryRange memory_range`（weight 填满）。
- 底部：`Button cancel`（取消）+ `Button ext_btn`（确认搜索，weight 各 1）。
- 按钮均 `textSize=@dimen/button_text_size`；48dp 触控高度。

### 5.3 hot_point_view（悬浮球）
- speed_dec / speed_inc：48×48dp，padding 0，margin 10dp，图标 `ic_fast_forward_*` / 减速图标。
- HotFrame：min 24dp；进度条 10dp 文本 `#80000000` 底。
- speed_text：padding 5dp 水平，背景 `@drawable/ic_gg` 类，字号 textAppearance 小。

### 5.4 service_saved_item（保存列表条目）
- 根 LinearLayout：gravity=center_vertical，`descendantFocusability=blocksDescendants`，高 wrap。
- 结构：`CheckBox ch`（宽 `@dimen/checkbox_item_width`=48dp 高 48dp，weight 0）→ `ImageButtonView freeze`（48dp，图标 `ic_nolock_24dp`）→ `WrapLayout`（weight=1，竖向）：
  - `name`（名称，"armor and defence"）
  - `address`（`A9CC4040:`，颜色 `@color/address`）
  - `value_hex`（`ABCD...h;`）、`value_rhex`（`...r;`）、`value_string`、`value_java`、`value_arm`（`MOV PC, LR;`，颜色 `@color/region_code`）、`value_thumb`、`value_arm8`（默认 GONE）、`addr_item_value`（`-3.1234567e-140` 值预览）
  - 右侧竖列：`addr_item_type`（"D"）、`addr_item_info`（"Jh"）
  - `addr_item_remove`（48dp 删除图标）
- 紧凑模式（`config_use_small_list_items`）：CheckBox 24dp、WrapLayout 横向、类型列收窄。

### 5.5 service_config（配置页，55KB 布局）
- 根 LinearLayout（vertical），含 5 个区块：`help`（帮助）、`donate`（捐赠）、`config_reset`（重置）、`config_exit`（退出）、`config_kill_game`（杀游戏）、`game_section`（游戏段）→ 随后是约 60 个配置行：`config_ranges`（区域）、`config_speedhack_intercept`（变速拦截）、`config_time_jump_panel`、`config_unrandomizer`、`config_hide_from_game`、`config_ptrace_bypass_mode`、`config_skip_memory`、`config_fast_freeze`、`config_freeze_interval`（seekbar，33ms 步进）、`config_autopause`、`config_saved_list_updates_interval`、`config_speeds`、`config_check_libs`、`config_ram`、`config_memory_access`、`config_deep_read`、`config_calls`、`config_waitpid`、`config_path`（路径）、`config_su`、`config_vspace_root`、`config_prevent_unload`、`config_use_notification`、`config_hot_key`、`config_history_limit`（seekbar）、`config_keyboard`（外置/内置）、`config_suggestions`、`config_ignore_unknown_chars`、`config_indent`、`config_visible_type`、`config_use_small_list_items`、`config_backgrounds`、`config_toolbars`、`config_toolbar_buttons`、`config_fill_toolbar`、`config_icons_size`（seekbar 12-?）、`config_icon`（悬浮球图标）、`config_icon_text`、`config_icon_example`、`hide_icons`、`config_acceleration`、`config_use_sound_effects`、`config_language`、`config_number_locale`、`config_disable_protection`、`config_clear_history`、`config_clear_defaults`、`config_last_stats`、`show_logcat`、`config_region_log`、`screenshot`、`about`、`selinux`、`config_xor_key`、`config_context_source`、`config_float_type`、`config_float_flags`、`config_kbd_small`、`config_time_jump_last`、`config_copy_params`、`record_logcat`。
- 第二屏 `service_config_hide`（config 隐藏项）、`service_config_icon`（图标/透明度/隐藏/大小配置）。

### 5.6 其他布局
- `service_editor`（编辑对话框）：ScrollView，`change_value`（修改值按钮行）、`value_row`（`type_hint` + `value` 输入 + `number_converter`）、`edit_step_row`（`edit_step_label`+`edit_step`+`step_converter`）、`add_not_replace`（CheckBox）。
- `service_address_item`（地址列表条目）：`ch`(CheckBox)、`address`、`value_hex/value_rhex/value_string/value_java/value_arm/value_thumb/value_arm8`、`addr_item_value`、`addr_item_type/info`、`addr_item_remove`。
- `service_memory_item`（内存编辑器条目）：`ch`、`address`、多值 TextView、`value_dword/value_float/value_double`。
- `service_busy_dialog`：进度对话框（`progress_bar_text/progress_bar/stage_layout/progress_bar_stage_text/progress_bar_stage/found_count/time/help_speed_up`）。
- `service_goto`：跳转（`addr`、`mem_edit_addr`、`region_from`、`history`、`cd/cb/ps/xa`）。
- `service_offset`：偏移计算器（`address/offset/hex/result/save` + 各类型值框）。
- `service_filter_search`：过滤搜索（`cAddrGreat/eAddrGreat/cAddrLess/eAddrLess/cValGreat/eValGreat/cValLess/eValLess/cType/sType/cFractional/sFractional/cPointer/sPointer/eMaxShow`）。
- `service_fuzzy_equal`：模糊等于（`count_fuzzy_equal` + seekbar）。
- `time_jump` / `time_jump_panel`：时间跳转（`time_jump`/`time_jump_last`）。
- `timer_header/timer_row/timer_table`：定时器表（4 列 app/java/system/other CheckBox + 行号）。
- `keyboard`（`KeyboardLayout`：`kbd_scroll` ScrollView / `kbd_frame` FrameLayout / `keyboard` InternalKeyboard）；XML 键盘定义（Keyboard 根，5 套：num/hex/fn/small/ext）。
- `show_app`（ShowApp 根）：app 选择。
- `main`（Activity 启动页）：`root/loader/installer/installing/progress_bar_text/progress_bar/abort_button/starter/front_text/language/help/fix_it/btn_start_usage/btn_stop_service/check/changelog`。
- `dialog_title`：自定义标题（`custom_title/alertTitle/subTitle`）。
- `alloc/dump/execute/logcat/memory_copy/memory_filter/option_edit/option_seek/path_selector/prompt/save/select_dialog_item/temp_path_config/unrand/history_item/path_item/path_suggestion`：脚本/内存工具对话框。

---

## 6. 页面交互关系（流程还原）

```
启动 App (MainActivity/ActivityMain)
  → ar.onCreate: 安装器/启动界面 (layout/main)
  → 选择 root/非root 模式
  → 启动 MainService (悬浮窗宿主)
      ├── HotPoint 悬浮球 (WindowManager 直挂, TYPE_APPLICATION_OVERLAY)
      │     ├── 拖动 (36dp 阈值)
      │     ├── 单击 → MainService.f() 显示主面板 Dialog (me)
      │     └── 长按 → 拖入游戏 / 菜单
      └── 主面板 (Dialog me + TabHost service_dialog)
            ├── [配置 tab]  ConfigListAdapter → service_config 列表
            │      └── 各项配置: 开关/单选/seekbar/文本/路径选择
            ├── [搜索 tab]  Searcher → service_searcher
            │      ├── 输入值/类型/范围 → 搜索 (Mock/真实)
            │      ├── 结果 → addr_list (service_address_item)
            │      │      ├── 单击选择 (CheckBox)
            │      │      ├── 长按 → ItemContextMenu (修改/添加/冻结/复制/跳转…)
            │      │      └── 多选 → 工具栏批量操作 (修改/添加/冻结)
            │      └── MemoryRange → 区域选择对话框
            ├── [保存 tab]  SavedListAdapter → service_saved_item
            │      ├── 冻结开关 (ImageButtonView freeze)
            │      ├── 删除 (确认对话框)
            │      ├── 长按 → ItemContextMenu
            │      └── 值实时更新 (android.c.e ← 冻结引擎回调)
            └── [内存 tab]  mem_listview (service_memory_item)
                   └── 内存编辑器 (地址跳转/值查看)
```
交互要点：
- **悬浮球 ↔ 主面板**：`HotPoint` 点击 → `MainService.f()`（显示 Dialog 主面板）；面板 `BACK`/关闭 → `MainService.g()`（收起面板，悬浮球回显）。`MainService.e()`（菜单键）在面板/搜索间切换。
- **搜索 → 结果**：`Searcher` 的确认按钮触发搜索，结果填充 `addr_list`（`service_address_item` 适配器 `AddressArrayAdapter`），`nothing_found_view` 控制空态。
- **结果 → 保存/冻结**：长按 `ItemContextMenu` 或工具栏按钮 → `SavedListAdapter.a(qx)` 加入保存列表；冻结项注册到 `ex` 引擎周期性写值。
- **键盘**：所有数值输入框（EditText 系列）经 `InternalKeyboard` 包裹，`KeyboardLayout` 在横竖屏切换时重排（竖屏键盘在下方、横屏在右侧）。

---

## 7. 逆向结论与实现建议（供独立重实现参考）

1. **架构**：Service 持有悬浮球（WindowManager 直挂）+ 主面板（Dialog + TabHost）。主面板用 Dialog 而非 WindowManager.addView，天然获得窗口管理与焦点语义 —— 独立实现时推荐同样的"悬浮球 Overlay + 主面板 Dialog"双窗口结构。
2. **窗口类型**：现代 Android（SDK≥26）必须用 `TYPE_APPLICATION_OVERLAY`（2038），低版本用 `TYPE_PHONE`(2002)；`FLAG_NOT_FOCUSABLE` 保证不抢焦点、不弹输入法（内建键盘方案可避免 IME 焦点问题）。
3. **信息密度**：全部触控目标 48dp 高、图标 24dp+12dp padding；工具栏 48dp；文本按钮紧凑、无大圆角卡片；背景半透明黑 `#c0000000`；分隔线 1dp 半透明白。
4. **数据模型**：`qx`（地址/值/类型/冻结标志）贯穿搜索→保存→冻结；区域表 `RegionList` 二分查找地址归属；类型枚举 Byte/Word/Dword/Qword/Float/Double/Xor。
5. **颜色语义**：`@color/address`（地址色）、`@color/region_code`（代码区）、`@color/type_*`（类型色）——值/类型按语义着色，而非单一前景色。
6. **内建键盘**：基于 `android.inputmethodservice.Keyboard` + XML 键盘表，多模式（数字/十六进制/函数/小键盘）动态切换，`key_width/key_height` dimen 控制键尺寸。独立实现可简化为一套 GridLayout 数字键盘（任务要求 4×4 布局）。
7. **不做的事**（本阶段）：不复制其混淆/反检测机制、不复制受保护资源（图标为 Material 风格，重新绘制/使用系统 drawable）、不复制其 root/内存读写实现。第一阶段全部 Mock。

**明确标注为推测/UNKNOWN 的项**：
- `sign_spinner` 的 `SystemSpinner` 弹出对话框样式（单选列表）为 GG 常规实现，未逐字节验证其主题。
- 键盘 `k_ext` 扩展键盘具体键位未逐键还原（混淆 XML 中 Keyboard 行无法 100% 映射），实现时按任务 §12 的 4×4 布局自定。
- `Searcher` 5 种模式的精确可见性矩阵 `F[5][11]` 已提取但部分区块用途（如 `cFractional/sFractional`）依赖业务语义，未逐项展开。
- 状态栏文本 `8.40.0 + Ch,Jh,...` 中的缩写对应 GG 功能开关，具体含义 UNKNOWN，重实现时用自有文本。
