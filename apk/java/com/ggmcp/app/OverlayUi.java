package com.ggmcp.app;

import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.BaseAdapter;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashSet;
import java.util.Locale;
import java.util.WeakHashMap;

/**
 * 悬浮窗 UI（GG 风格，4 标签：搜索/内存/保存/设置）。
 * - 断点/日志/进程不再占标签：断点并入内存页，日志走设置页入口 + 浮动通知，进程选择在设置页。
 * - 事件流（cmd_start/cmd_end/watch_hit/search_update/results_update/session_change/annotate）
 *   实时显示在操作日志对话框 + 浮动通知。
 */
public class OverlayUi {

    public static final int LOG_CMD = 1;      // 命令（cmd_start/end/search_update/session）
    public static final int LOG_BP = 2;       // 断点命中
    public static final int LOG_ANNOT = 3;    // 标注/保存

    public static final int NOTIF_CMD = 1;    // 命令（灰）
    public static final int NOTIF_BP = 2;     // 断点命中（红，可点击）
    public static final int NOTIF_ANNOT = 3;  // 标注新增（绿）
    public static final int NOTIF_ERR = 4;    // 错误（橙）

    private static final String[] TYPE_NAMES = {"BYTE", "WORD", "DWORD", "QWORD", "FLOAT", "DOUBLE", "AUTO"};
    private static final String[] REGION_NAMES = {"匿名", "全部", "Java堆", "C++堆", "栈", "代码段", "自定义范围"};
    private static final int[] REGION_CODES = {WireClient.REGION_ANONYMOUS, WireClient.REGION_ALL,
            WireClient.REGION_ALL, WireClient.REGION_ALL, WireClient.REGION_ALL, WireClient.REGION_ALL, WireClient.REGION_ALL};
    private static final String[] ALIGN_NAMES = {"1字节", "2字节", "4字节", "8字节", "未对齐"};
    private static final String[] SAVE_TYPE_NAMES = {"DWORD", "FLOAT", "BYTE", "WORD", "QWORD", "DOUBLE"};
    private static final int[] SAVE_TYPE_CODES = {WireClient.VT_DWORD, WireClient.VT_FLOAT,
            WireClient.VT_BYTE, WireClient.VT_WORD, WireClient.VT_QWORD, WireClient.VT_DOUBLE};

    private final Context ctx;
    private final WireClient client;
    private final Handler h = new Handler(Looper.getMainLooper());
    private final View root;
    private SharedPreferences prefs;

    // 面板
    private View panelSearch, panelMemory, panelSave, panelSettings;
    private final TextView[] tabs = new TextView[4];
    private TextView titleText, statusResult, statusSelected, statusFrozen;
    private View notifBar;
    private Runnable notifDismiss;

    // 会话
    private int sessionPid = -1;
    private String sessionName = "";

    // ---------------- 搜索面板 ----------------
    private final TextView[] typeBtns = new TextView[7];
    private int curType = 2;                 // DWORD
    private int curRegion = WireClient.REGION_ANONYMOUS;
    private boolean fuzzyMode = false;
    private EditText edValue, edMin, edMax;
    private TextView infoText, btnSearch;
    private View advancedRow, refineRow;
    private Spinner regionSpinner, alignSpinner;
    private ListView resultList;
    private ResultAdapter resultAdapter;
    private final HashSet<String> resultSelected = new HashSet<>();
    private final HashSet<String> resultFrozen = new HashSet<>();
    private final ArrayList<long[]> regionRanges = new ArrayList<>();
    private final ArrayList<String> regionTags = new ArrayList<>();

    // ---------------- 内存面板 ----------------
    private EditText memAddr;
    private TextView memInfo;
    private ListView memList;
    private MemAdapter memAdapter;
    private long memBase = 0;
    private byte[] memData;
    private int selRow = 0;
    private View opBar;
    private final HashSet<String> bpAddrs = new HashSet<>();

    // ---------------- 保存面板 ----------------
    private final ArrayList<JSONObject> saves = new ArrayList<>();
    private ListView saveList;
    private SaveAdapter saveAdapter;
    private final HashSet<String> saveSelected = new HashSet<>();
    private final HashSet<String> saveFrozen = new HashSet<>();

    // ---------------- 日志 ----------------
    private ListView logList;
    private LogAdapter logAdapter;
    private CheckBox filterAll, filterCmd, filterBp, filterAnnot;
    private int hitCount = 0;
    private final ArrayList<String> hitTitles = new ArrayList<>();
    private final ArrayList<String> hitBodies = new ArrayList<>();
    private String lastHitAddr = "";

    // ---------------- 进程对话框 ----------------
    private EditText filterText;
    private ListView processList;
    private TextView sessionText;
    private final ArrayList<String> procRows = new ArrayList<>();
    private final ArrayList<Integer> procPids = new ArrayList<>();
    private AlertDialog procDialog;

    // ---------------- 设置 ----------------
    private TextView statusKernel, tvProcessName;
    private SeekBar seekOpacity;
    private TextView tvOpacity;
    private Switch swVibrate, swNotify, swBoot;
    private final WeakHashMap<TextView, Float> baseFontSizes = new WeakHashMap<>();
    private float fontScale = 1.0f;

    /** 设置页 → 服务的回调（悬浮球大小、字体缩放） */
    public interface SettingsListener {
        void onBallSizeChanged(int dp);
        void onFontScaleChanged(float scale);
    }

    private SettingsListener settingsListener;

    public void setSettingsListener(SettingsListener l) { settingsListener = l; }

    public OverlayUi(Context ctx, WireClient client) {
        this.ctx = ctx;
        this.client = client;
        prefs = ctx.getSharedPreferences("ggmcp", Context.MODE_PRIVATE);

        root = LayoutInflater.from(ctx).inflate(R.layout.float_window, null);
        LayoutInflater inf = LayoutInflater.from(ctx);
        panelSearch = inf.inflate(R.layout.panel_search, null);
        panelMemory = inf.inflate(R.layout.panel_memory, null);
        panelSave = inf.inflate(R.layout.panel_annotation, null);
        panelSettings = inf.inflate(R.layout.panel_settings, null);
        ViewGroup content = root.findViewById(R.id.contentArea);
        content.addView(panelSearch);
        content.addView(panelMemory);
        content.addView(panelSave);
        content.addView(panelSettings);

        tabs[0] = root.findViewById(R.id.tabSearch);
        tabs[1] = root.findViewById(R.id.tabMemory);
        tabs[2] = root.findViewById(R.id.tabSave);
        tabs[3] = root.findViewById(R.id.tabSettings);
        for (int i = 0; i < 4; i++) {
            final int fi = i;
            tabs[i].setOnClickListener(v -> switchTab(fi));
        }
        titleText = root.findViewById(R.id.titleText);
        statusResult = root.findViewById(R.id.statusResult);
        statusSelected = root.findViewById(R.id.statusSelected);
        statusFrozen = root.findViewById(R.id.statusFrozen);
        notifBar = root.findViewById(R.id.notifBar);

        initSearchPanel();
        initMemoryPanel();
        initSavePanel();
        initSettingsPanel();
        logAdapter = new LogAdapter();   // 事件流随时可能写入，先建实例
        switchTab(0);
        loadSaves();
        refreshStatus();
        statusBarUpdate();
    }

    public View getRootView() { return root; }

    private void switchTab(int idx) {
        panelSearch.setVisibility(idx == 0 ? View.VISIBLE : View.GONE);
        panelMemory.setVisibility(idx == 1 ? View.VISIBLE : View.GONE);
        panelSave.setVisibility(idx == 2 ? View.VISIBLE : View.GONE);
        panelSettings.setVisibility(idx == 3 ? View.VISIBLE : View.GONE);
        for (int i = 0; i < 4; i++) {
            tabs[i].setTextColor(i == idx ? 0xFFFFFFFF : 0xFFB0B0B0);
            tabs[i].setBackgroundResource(i == idx ? R.drawable.tab_sel : R.drawable.tab_norm);
        }
    }

    // ================= 浮动通知（替代独立日志页） =================

    public void showNotif(final int kind, final String text, final long ms, final Runnable onClick) {
        h.post(() -> {
            if (notifBar == null) return;
            notifBar.removeCallbacks(notifDismiss);
            int bg;
            int tc;
            switch (kind) {
                case NOTIF_BP:
                    bg = R.drawable.alert_bg;            // 红底 → 深色文字保证对比度
                    tc = 0xFF101018;
                    break;
                case NOTIF_ANNOT:
                    bg = R.drawable.notif_annot_bg;      // 绿底 → 深色文字
                    tc = 0xFF101018;
                    break;
                case NOTIF_ERR:
                    bg = R.drawable.notif_err_bg;
                    tc = 0xFFFFFFFF;
                    break;
                default:
                    bg = R.drawable.notif_cmd_bg;
                    tc = 0xFFFFFFFF;
            }
            notifBar.setBackgroundResource(bg);
            ((TextView) notifBar).setTextColor(tc);
            ((TextView) notifBar).setText(text);
            notifBar.setVisibility(View.VISIBLE);
            notifBar.setAlpha(0f);
            notifBar.animate().alpha(1f).setDuration(200).start();
            notifBar.setOnClickListener(v -> {
                notifBar.removeCallbacks(notifDismiss);
                hideNotifNow();
                if (onClick != null) onClick.run();
            });
            notifDismiss = () -> hideNotifNow();
            notifBar.postDelayed(notifDismiss, ms);
        });
    }

    private void hideNotifNow() {
        if (notifBar == null) return;
        notifBar.animate().alpha(0f).setDuration(200).withEndAction(() -> {
            if (notifBar != null) notifBar.setVisibility(View.GONE);
        }).start();
    }

    // ================= 事件流（实时监控核心） =================

    /** 由 FloatWindowService 主线程回调。 */
    public void appendEvent(JSONObject ev) {
        String type = ev.optString("type");
        long ts = ev.optLong("ts");
        String time = new SimpleDateFormat("HH:mm:ss", Locale.US).format(new Date(ts));
        JSONObject d = ev.optJSONObject("data");
        if (d == null) d = new JSONObject();
        switch (type) {
            case "cmd_start": {
                String cmd = d.optString("cmd");
                JSONObject params = d.optJSONObject("params");
                String p = params != null ? params.toString() : "{}";
                logAdapter.add(new LogItem(time, cmd, p, LOG_CMD, false, false));
                break;
            }
            case "cmd_end": {
                String cmd = d.optString("cmd");
                boolean ok = d.optBoolean("success");
                long dur = d.optLong("duration_ms");
                JSONObject res = d.optJSONObject("result");
                String rs = res != null ? res.toString() : "{}";
                if (rs.length() > 240) rs = rs.substring(0, 240) + "…";
                String line = ok ? "✓ " + cmd + " (" + dur + "ms)  " + rs
                        : "✗ " + cmd + " 失败: " + rs;
                logAdapter.add(new LogItem(time, cmd, line, LOG_CMD, ok, !ok));
                // 命令完成浮动通知（命令类）
                if (ok) {
                    String shortRes = rs.length() > 24 ? rs.substring(0, 24) + "…" : rs;
                    showNotif(NOTIF_CMD, cmd + " → " + shortRes, 1500, null);
                }
                break;
            }
            case "search_update": {
                long c = d.optLong("count"), pc = d.optLong("prev_count");
                logAdapter.add(new LogItem(time, "搜索", "搜索结果: " + pc + " → " + c, LOG_CMD, false, false));
                showNotif(NOTIF_CMD, "搜索 → " + c + " 条", 1500, null);
                break;
            }
            case "results_update": {
                handleResultsUpdate(d);
                break;
            }
            case "session_change": {
                String st = d.optString("state");
                long pid = d.optLong("pid");
                String name = d.optString("name");
                String line = "attached".equals(st) ? "已附加 pid " + pid + " (" + name + ")"
                        : "已分离 pid " + pid;
                sessionPid = "attached".equals(st) ? (int) pid : -1;
                sessionName = "attached".equals(st) ? name : "";
                logAdapter.add(new LogItem(time, "会话", line, LOG_CMD, false, false));
                titleText.setText(sessionPid > 0 ? sessionName : "未选择进程");
                tvProcessName.setText(sessionPid > 0 ? sessionName : "未选择进程");
                if (sessionPid > 0) loadRegions();
                break;
            }
            case "watch_hit": {
                String module = d.optString("module");
                String off = d.optString("module_offset");
                String pc = d.optString("pc");
                String x0 = d.optString("r0"), x1 = d.optString("r1");
                String title = "⚡ 断点命中! " + (module.isEmpty() ? d.optString("addr") : module + "+" + off);
                String body = "pc=" + pc + "  x0=" + x0 + "  x1=" + x1;
                logAdapter.add(new LogItem(time, "断点命中", title + "  " + body, LOG_BP, false, false));
                hitTitles.add(0, title);
                hitBodies.add(0, body + "  [" + time + "]");
                if (hitTitles.size() > 300) { hitTitles.remove(hitTitles.size() - 1); hitBodies.remove(hitBodies.size() - 1); }
                hitCount++;
                lastHitAddr = d.optString("addr", "");
                statusBarUpdate();
                // 红通知，点击跳转内存页对应地址
                showNotif(NOTIF_BP, title, 3000, () -> {
                    switchTab(1);
                    try {
                        if (!lastHitAddr.isEmpty()) {
                            long a = Long.decode(lastHitAddr);
                            memLoad(a - 0x10);
                        }
                    } catch (Exception ignored) {}
                });
                break;
            }
            case "annotate": {
                handleAnnotEvent(d);
                break;
            }
            default: {
                logAdapter.add(new LogItem(time, type, d.toString(), LOG_CMD, false, false));
            }
        }
    }

    public void setConnState(boolean connected, String error) {
        // 连接状态由悬浮球边框 + 通知栏体现
    }

    private void statusBarUpdate() {
        int n = resultAdapter != null ? resultAdapter.getCount() : 0;
        int sel = resultSelected.size() + saveSelected.size();
        int fz = resultFrozen.size() + saveFrozen.size();
        statusResult.setText("结果: " + n);
        statusSelected.setText("已选: " + sel);
        statusFrozen.setText("冻结: " + fz);
    }

    // ================= 通用后台调用 =================

    interface CallR { String run(WireClient c) throws Exception; }

    /** 后台线程调 agentd，主线程回调；错误自动弹橙色通知。 */
    private void bg(final java.util.function.Consumer<String> done, final CallR rwp) {
        new Thread(() -> {
            if (!client.isConnected()) client.connect(EngineManager.PORT);
            String result;
            try {
                result = rwp.run(client);
            } catch (Exception e) {
                result = "err: " + e.getMessage();
            }
            final String res = result;
            h.post(() -> {
                if (res != null && res.startsWith("err")) {
                    showNotif(NOTIF_ERR, "✗ " + res, 3000, null);
                }
                if (done != null) done.accept(res);
            });
        }).start();
    }

    // ================= 搜索面板 =================

    private void initSearchPanel() {
        edValue = panelSearch.findViewById(R.id.edValue);
        edMin = panelSearch.findViewById(R.id.edMin);
        edMax = panelSearch.findViewById(R.id.edMax);
        infoText = panelSearch.findViewById(R.id.infoText);
        btnSearch = panelSearch.findViewById(R.id.btnSearch);
        advancedRow = panelSearch.findViewById(R.id.advancedRow);
        refineRow = panelSearch.findViewById(R.id.refineRow);
        resultList = panelSearch.findViewById(R.id.resultList);
        resultAdapter = new ResultAdapter();
        resultList.setAdapter(resultAdapter);
        resultList.setOnItemClickListener((parent, view, position, id) -> {
            if (position < resultAdapter.addrs.size()) editResult(position);
        });
        resultList.setOnItemLongClickListener((parent, view, position, id) -> {
            if (position < resultAdapter.addrs.size()) resultMenu(position);
            return true;
        });

        // 类型方块
        int[] typeIds = {R.id.type0, R.id.type1, R.id.type2, R.id.type3, R.id.type4, R.id.type5, R.id.type6};
        for (int i = 0; i < 7; i++) {
            typeBtns[i] = panelSearch.findViewById(typeIds[i]);
            final int fi = i;
            typeBtns[i].setOnClickListener(v -> selectType(fi));
        }
        selectType(2);

        // 范围
        regionSpinner = panelSearch.findViewById(R.id.regionSpinner);
        ArrayAdapter<String> regAd = new ArrayAdapter<>(ctx,
                android.R.layout.simple_spinner_dropdown_item, REGION_NAMES);
        regAd.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        regionSpinner.setAdapter(regAd);
        regionSpinner.setSelection(0);
        regionSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) { curRegion = REGION_CODES[pos]; }
            @Override public void onNothingSelected(AdapterView<?> p) {}
        });

        // 对齐（引擎当前不支持对齐参数，仅 UI 展示）
        alignSpinner = panelSearch.findViewById(R.id.alignSpinner);
        ArrayAdapter<String> alAd = new ArrayAdapter<>(ctx,
                android.R.layout.simple_spinner_dropdown_item, ALIGN_NAMES);
        alAd.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        alignSpinner.setAdapter(alAd);
        alignSpinner.setSelection(2);

        panelSearch.findViewById(R.id.btnModeExact).setOnClickListener(v -> setFuzzyMode(false));
        panelSearch.findViewById(R.id.btnModeFuzzy).setOnClickListener(v -> setFuzzyMode(true));

        btnSearch.setOnClickListener(v -> {
            if (fuzzyMode) fuzzyInit(); else exactSearch();
        });
        panelSearch.findViewById(R.id.btnReset).setOnClickListener(v -> resetResults());
        panelSearch.findViewById(R.id.btnMore).setOnClickListener(v -> {
            advancedRow.setVisibility(advancedRow.getVisibility() == View.VISIBLE ? View.GONE : View.VISIBLE);
        });
        panelSearch.findViewById(R.id.btnRange).setOnClickListener(v -> rangeSearch());
        panelSearch.findViewById(R.id.opIncreased).setOnClickListener(v -> refineOp(WireClient.REFINE_INCREASED));
        panelSearch.findViewById(R.id.opDecreased).setOnClickListener(v -> refineOp(WireClient.REFINE_DECREASED));
        panelSearch.findViewById(R.id.opUnchanged).setOnClickListener(v -> refineOp(WireClient.REFINE_UNCHANGED));
        panelSearch.findViewById(R.id.opChanged).setOnClickListener(v -> refineOp(WireClient.REFINE_CHANGED));
    }

    private void selectType(int i) {
        curType = i;
        for (int k = 0; k < 7; k++) {
            typeBtns[k].setTextColor(k == i ? 0xFF101010 : 0xFFB0B0B0);
            typeBtns[k].setBackgroundResource(k == i ? R.drawable.type_sel : R.drawable.type_norm);
        }
    }

    private void setFuzzyMode(boolean f) {
        fuzzyMode = f;
        edValue.setVisibility(f ? View.GONE : View.VISIBLE);
        btnSearch.setText(f ? "模糊初始化" : "搜索");
        ((TextView) panelSearch.findViewById(R.id.btnModeExact)).setTextColor(f ? 0xFFB0B0B0 : 0xFF101010);
        panelSearch.findViewById(R.id.btnModeExact).setBackgroundResource(f ? R.drawable.btn_dark : R.drawable.btn_primary);
        ((TextView) panelSearch.findViewById(R.id.btnModeFuzzy)).setTextColor(f ? 0xFF101010 : 0xFFB0B0B0);
        panelSearch.findViewById(R.id.btnModeFuzzy).setBackgroundResource(f ? R.drawable.btn_primary : R.drawable.btn_dark);
    }

    private int engineType() {
        return curType < 6 ? curType + 1 : WireClient.VT_DWORD;   // AUTO → DWORD 回退
    }

    /** 未附加进程时提示并中断（设计：搜索/内存/保存页显示"请先在设置中选择进程"） */
    private boolean requireSession() {
        if (sessionPid <= 0) {
            showNotif(NOTIF_ERR, "✗ 未附加进程，请先在设置中选择进程", 3000, null);
            return false;
        }
        return true;
    }

    private void exactSearch() {
        if (!requireSession()) return;
        final String v = edValue.getText().toString().trim();
        if (v.isEmpty()) { toast("输入值"); return; }
        if (curType == 6) showNotif(NOTIF_CMD, "AUTO 未支持，已按 DWORD 搜索", 2000, null);
        bg(res -> {
            if (res != null && res.startsWith("count")) {
                String[] parts = res.split("=");
                if (parts.length == 2) {
                    long c = Long.parseLong(parts[1].split(" ")[0]);
                    infoText.setText("命中 " + c + " 个地址");
                    refineRow.setVisibility(c > 0 ? View.VISIBLE : View.GONE);
                }
            }
            statusBarUpdate();
        }, c -> {
            JSONObject p = new JSONObject();
            p.put("value", v);
            p.put("type", engineType());
            p.put("region", curRegion);
            JSONObject r = c.call(WireClient.CMD_SEARCH, p);
            long count = r.optLong("count");
            if (count >= 0 && count <= 200) loadResults(c);
            return "count=" + count + (r.optBoolean("truncated") ? " (截断)" : "");
        });
    }

    private void fuzzyInit() {
        if (!requireSession()) return;
        bg(res -> {
            if (res != null && res.startsWith("elements")) {
                String[] parts = res.split("=");
                if (parts.length == 2) {
                    infoText.setText("模糊搜索初始化: " + parts[1] + " 元素，游戏内变化后点 refine");
                    refineRow.setVisibility(View.VISIBLE);
                }
            }
        }, c -> {
            JSONObject p = new JSONObject();
            p.put("type", engineType());
            p.put("region", curRegion);
            JSONObject r = c.call(WireClient.CMD_SEARCH_UNKNOWN_INIT, p);
            return "elements=" + r.optLong("elements");
        });
    }

    private void rangeSearch() {
        if (!requireSession()) return;
        final String s1 = edMin.getText().toString().trim();
        final String s2 = edMax.getText().toString().trim();
        if (s1.isEmpty() || s2.isEmpty()) { toast("填写 min/max"); return; }
        bg(res -> {
            if (res != null && res.startsWith("count")) {
                String[] parts = res.split("=");
                if (parts.length == 2) {
                    long c = Long.parseLong(parts[1].split(" ")[0]);
                    infoText.setText("命中 " + c + " 个地址");
                    refineRow.setVisibility(c > 0 ? View.VISIBLE : View.GONE);
                }
            }
        }, c -> {
            JSONObject p = new JSONObject();
            p.put("min", s1);
            p.put("max", s2);
            p.put("type", engineType());
            p.put("region", curRegion);
            JSONObject r = c.call(WireClient.CMD_SEARCH_RANGE, p);
            long count = r.optLong("count");
            if (count >= 0 && count <= 200) loadResults(c);
            return "count=" + count;
        });
    }

    private void refineOp(final int op) {
        if (!requireSession()) return;
        bg(res -> {
            if (res != null && res.startsWith("remaining")) {
                String[] parts = res.split("=");
                if (parts.length == 2) {
                    long rem = Long.parseLong(parts[1].split(" ")[0]);
                    infoText.setText("refine 剩余 " + rem + " 个地址");
                    refineRow.setVisibility(rem > 0 ? View.VISIBLE : View.GONE);
                }
            }
        }, c -> {
            JSONObject p = new JSONObject();
            p.put("op", op);
            JSONObject r = c.call(WireClient.CMD_SEARCH_REFINE, p);
            long rem = r.optLong("remaining");
            if (rem >= 0 && rem <= 200) loadResults(c);
            return "remaining=" + rem;
        });
    }

    private void resetResults() {
        bg(res -> {
            resultAdapter.clearData();
            resultSelected.clear();
            resultFrozen.clear();
            refineRow.setVisibility(View.GONE);
            infoText.setText("搜索结果将显示在这里");
            statusBarUpdate();
        }, c -> {
            c.call(WireClient.CMD_CLEAR_RESULTS, new JSONObject());
            return "cleared";
        });
    }

    private void loadResults(final WireClient c) {
        try {
            JSONObject p = new JSONObject();
            p.put("offset", 0);
            p.put("limit", 300);
            p.put("type", engineType());
            JSONObject r = c.call(WireClient.CMD_GET_RESULTS, p);
            final ArrayList<String> na = new ArrayList<>();
            final ArrayList<String> nv = new ArrayList<>();
            JSONArray arr = r.optJSONArray("results");
            if (arr != null) {
                for (int i = 0; i < arr.length(); i++) {
                    JSONObject o = arr.optJSONObject(i);
                    if (o == null) continue;
                    na.add(o.optString("addr"));
                    nv.add(o.optString("value"));
                }
            }
            h.post(() -> {
                resultAdapter.setData(na, nv);
                infoText.setText("命中 " + na.size() + " 个地址");
                refineRow.setVisibility(na.size() > 0 ? View.VISIBLE : View.GONE);
                statusBarUpdate();
            });
        } catch (Exception ignored) {}
    }

    /** AI(PC端) 搜索结果同步事件：刷新搜索页列表 + 冻结勾选 + 状态栏。 */
    private void handleResultsUpdate(JSONObject d) {
        try {
            final long count = d.optLong("count");
            final long frozenCount = d.optLong("frozen_count");
            JSONArray arr = d.optJSONArray("results");
            final ArrayList<String> na = new ArrayList<>();
            final ArrayList<String> nv = new ArrayList<>();
            final HashSet<String> nf = new HashSet<>();
            if (arr != null) {
                for (int i = 0; i < arr.length(); i++) {
                    JSONObject o = arr.optJSONObject(i);
                    if (o == null) continue;
                    na.add(o.optString("addr"));
                    nv.add(o.optString("value"));
                    if (o.optBoolean("frozen")) nf.add(o.optString("addr"));
                }
            }
            h.post(() -> {
                resultAdapter.setData(na, nv);
                resultFrozen.clear();
                resultFrozen.addAll(nf);
                infoText.setText("命中 " + count + " 个地址" + (frozenCount > 0 ? " · 冻结 " + frozenCount : ""));
                refineRow.setVisibility(count > 0 ? View.VISIBLE : View.GONE);
                statusBarUpdate();
            });
        } catch (Exception ignored) {}
    }

    private void editResult(final int index) {
        final String addr = resultAdapter.addrs.get(index);
        final String curVal = index < resultAdapter.values.size() ? resultAdapter.values.get(index) : "";
        final EditText val = new EditText(ctx);
        val.setHint("新值（当前 " + TYPE_NAMES[curType] + "）");
        val.setTextColor(0xFFE0E0E0);
        val.setInputType(InputType.TYPE_CLASS_TEXT);
        val.setText(curVal);
        LinearLayout ll = new LinearLayout(ctx);
        ll.setOrientation(LinearLayout.VERTICAL);
        ll.setPadding(30, 10, 30, 0);
        ll.addView(val);
        new OverlayDialogBuilder(ctx)
                .setTitle("修改 " + addr)
                .setView(ll)
                .setPositiveButton("写入", (d, w) -> {
                    final String nv = val.getText().toString().trim();
                    if (nv.isEmpty()) return;
                    writeAddr(addr, nv);
                })
                .setNeutralButton("读回", (d, w) -> readAddr(addr))
                .setNegativeButton("取消", null).show();
    }

    private void writeAddr(final String addr, final String nv) {
        bg(res -> {
            resultAdapter.notifyDataSetChanged();
            statusBarUpdate();
        }, c -> {
            JSONObject p = new JSONObject();
            p.put("addr", addr);
            p.put("value", nv);
            p.put("type", engineType());
            JSONObject r = c.call(WireClient.CMD_WRITE, p);
            return "bytes=" + r.optLong("bytes");
        });
    }

    private void readAddr(final String addr) {
        bg(res -> {
            if (res != null && !res.startsWith("err")) showNotif(NOTIF_CMD, res, 2000, null);
        }, c -> {
            JSONObject p = new JSONObject();
            p.put("addr", addr);
            p.put("type", engineType());
            JSONObject r = c.call(WireClient.CMD_READ, p);
            return addr + " = " + r.optString("value") + " " + r.optString("value_hex");
        });
    }

    /** 结果行长按菜单 */
    private void resultMenu(final int index) {
        final String addr = resultAdapter.addrs.get(index);
        final String value = index < resultAdapter.values.size() ? resultAdapter.values.get(index) : "";
        final String[] items = {"修改", "冻结", "添加到保存", "跳转内存", "删除"};
        new OverlayDialogBuilder(ctx)
                .setTitle(addr)
                .setItems(items, (d, w) -> {
                    switch (w) {
                        case 0: editResult(index); break;
                        case 1:
                            boolean on = !resultFrozen.contains(addr);
                            freezeResult(addr, value, on);
                            break;
                        case 2: addSaveDialog(addr, value); break;
                        case 3:
                            try {
                                long a = Long.decode(addr);
                                switchTab(1);
                                memLoad(a);
                            } catch (Exception e) { toast("地址格式错误"); }
                            break;
                        case 4:
                            resultAdapter.removeAt(index);
                            statusBarUpdate();
                            break;
                    }
                })
                .setNegativeButton("取消", null).show();
    }

    private void freezeResult(final String addr, final String value, final boolean on) {
        bg(res -> {
            if (on) resultFrozen.add(addr); else resultFrozen.remove(addr);
            resultAdapter.notifyDataSetChanged();
            statusBarUpdate();
        }, c -> {
            JSONObject p = new JSONObject();
            if (on) {
                p.put("addr", addr);
                p.put("value", value == null || value.isEmpty() ? "0" : value);
                p.put("type", engineType());
                c.call(WireClient.CMD_FREEZE, p);
                return "frozen";
            } else {
                p.put("addr", addr);
                c.call(WireClient.CMD_UNFREEZE, p);
                return "unfrozen";
            }
        });
    }

    // ================= 内存面板（hex 编辑器 + 断点） =================

    private void initMemoryPanel() {
        memAddr = panelMemory.findViewById(R.id.memAddr);
        memInfo = panelMemory.findViewById(R.id.memInfo);
        memList = panelMemory.findViewById(R.id.memList);
        memAdapter = new MemAdapter();
        memList.setAdapter(memAdapter);
        memList.setOnItemClickListener((parent, view, position, id) -> selectMemRow(position));
        memList.setOnItemLongClickListener((parent, view, position, id) -> {
            selRow = position;
            opBar.setVisibility(View.VISIBLE);
            return true;
        });

        panelMemory.findViewById(R.id.btnMemPrev).setOnClickListener(v -> { memLoad(memBase - 0x10); });
        panelMemory.findViewById(R.id.btnMemNext).setOnClickListener(v -> { memLoad(memBase + 0x10); });
        panelMemory.findViewById(R.id.btnMemJump).setOnClickListener(v -> {
            try {
                memLoad(Long.decode(memAddr.getText().toString().trim()));
            } catch (Exception e) { toast("地址格式错误"); }
        });
        panelMemory.findViewById(R.id.btnMemAdd).setOnClickListener(v -> {
            if (memData == null) return;
            long a = memBase + selRow * 16;
            addSaveDialog("0x" + Long.toHexString(a), "");
        });
        opBar = panelMemory.findViewById(R.id.opBar);
        panelMemory.findViewById(R.id.btnOpModify).setOnClickListener(v -> { opBar.setVisibility(View.GONE); memModify(); });
        panelMemory.findViewById(R.id.btnOpBp).setOnClickListener(v -> { opBar.setVisibility(View.GONE); memBpDialog(); });
        panelMemory.findViewById(R.id.btnOpSearch).setOnClickListener(v -> { opBar.setVisibility(View.GONE); memSearchValue(); });
        panelMemory.findViewById(R.id.btnOpSave).setOnClickListener(v -> { opBar.setVisibility(View.GONE); memSaveCurrent(); });
        panelMemory.findViewById(R.id.btnOpJump).setOnClickListener(v -> { opBar.setVisibility(View.GONE); memJumpPtr(); });
        panelMemory.findViewById(R.id.btnOpClose).setOnClickListener(v -> opBar.setVisibility(View.GONE));
    }

    private long selAddr() { return memBase + selRow * 16; }

    private void selectMemRow(int position) {
        selRow = position;
        memAdapter.notifyDataSetChanged();
        if (memData == null) return;
        long a = selAddr();
        StringBuilder sb = new StringBuilder();
        sb.append(String.format("选中: 0x%x", a));
        if (a + 4 <= memBase + memData.length) {
            int off = (int) (a - memBase);
            long dw = (memData[off] & 0xFFL) | ((memData[off + 1] & 0xFFL) << 8)
                    | ((memData[off + 2] & 0xFFL) << 16) | ((memData[off + 3] & 0xFFL) << 24);
            sb.append("  DWORD=").append(dw);
        }
        memInfo.setText(sb.toString());
    }

    private void memLoad(final long base) {
        bg(res -> {
            memAddr.setText("0x" + Long.toHexString(base));
            memInfo.setText(String.format("0x%x (256 字节)", memBase));
            selectMemRow(selRow);
        }, c -> {
            JSONObject p = new JSONObject();
            p.put("addr", "0x" + Long.toHexString(base));
            p.put("len", 256);
            JSONObject r = c.call(WireClient.CMD_HEX_DUMP, p);
            String hex = r.optString("hex");
            byte[] data = new byte[hex.length() / 2];
            for (int i = 0; i < data.length; i++) {
                data[i] = (byte) Integer.parseInt(hex.substring(i * 2, i * 2 + 2), 16);
            }
            memBase = base;
            memData = data;
            h.post(() -> memAdapter.notifyDataSetChanged());
            return "ok";
        });
    }

    private void memModify() {
        final long a = selAddr();
        final EditText val = new EditText(ctx);
        val.setHint("新值");
        val.setTextColor(0xFFE0E0E0);
        LinearLayout ll = new LinearLayout(ctx);
        ll.setOrientation(LinearLayout.VERTICAL);
        ll.setPadding(30, 10, 30, 0);
        ll.addView(val);
        new OverlayDialogBuilder(ctx)
                .setTitle("修改 0x" + Long.toHexString(a))
                .setView(ll)
                .setPositiveButton("写入", (d, w) -> {
                    final String nv = val.getText().toString().trim();
                    if (nv.isEmpty()) return;
                    final int t = engineType();
                    bg(res -> {
                        memLoad(memBase);
                    }, c -> {
                        JSONObject p = new JSONObject();
                        p.put("addr", "0x" + Long.toHexString(a));
                        p.put("value", nv);
                        p.put("type", t);
                        JSONObject r = c.call(WireClient.CMD_WRITE, p);
                        return "bytes=" + r.optLong("bytes");
                    });
                })
                .setNegativeButton("取消", null).show();
    }

    /** 硬件断点对话框（读/写/读写/执行） */
    private void memBpDialog() {
        final long a = selAddr();
        final int[] BP_TYPES = {1, 2, 3, 4};   // HW_BP_TYPE_R/W/RW/X
        final String[] BP_NAMES = {"读", "写", "读写", "执行"};
        final int[] chosen = {1};           // 默认写
        final TextView[] btns = new TextView[4];
        LinearLayout ll = new LinearLayout(ctx);
        ll.setOrientation(LinearLayout.HORIZONTAL);
        ll.setPadding(20, 10, 20, 0);
        for (int i = 0; i < 4; i++) {
            final int fi = i;
            btns[i] = new TextView(ctx);
            btns[i].setText(BP_NAMES[i]);
            btns[i].setGravity(android.view.Gravity.CENTER);
            btns[i].setPadding(dp(8), dp(8), dp(8), dp(8));
            btns[i].setBackgroundResource(i == chosen[0] ? R.drawable.btn_primary : R.drawable.btn_dark);
            btns[i].setTextColor(i == chosen[0] ? 0xFFFFFFFF : 0xFFB0B0B0);
            btns[i].setOnClickListener(v -> {
                chosen[0] = fi;
                for (int k = 0; k < 4; k++) {
                    btns[k].setBackgroundResource(k == fi ? R.drawable.btn_primary : R.drawable.btn_dark);
                    btns[k].setTextColor(k == fi ? 0xFF101010 : 0xFFB0B0B0);
                }
            });
            ll.addView(btns[i], new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        }
        new OverlayDialogBuilder(ctx)
                .setTitle("断点 0x" + Long.toHexString(a) + "（读/写/读写/执行）")
                .setView(ll)
                .setPositiveButton("设置", (d, w) -> {
                    final int bt = BP_TYPES[chosen[0]];
                    bg(res -> {
                        if (res != null && res.startsWith("active")) {
                            bpAddrs.add("0x" + Long.toHexString(a));
                            memAdapter.notifyDataSetChanged();
                            showNotif(NOTIF_CMD, "断点已设置: 0x" + Long.toHexString(a), 2000, null);
                        }
                    }, c -> {
                        JSONObject p = new JSONObject();
                        p.put("op", "set");
                        p.put("addr", "0x" + Long.toHexString(a));
                        p.put("type", bt);
                        p.put("len", 4);
                        JSONObject r = c.call(WireClient.CMD_WATCH, p);
                        return "active=" + r.optBoolean("active") + " threads=" + r.optInt("threads");
                    });
                })
                .setNegativeButton("取消", null).show();
    }

    /** 以选中地址当前值为条件搜内存 */
    private void memSearchValue() {
        final long a = selAddr();
        final int t = engineType();
        bg(res -> {
            if (res != null && res.startsWith("val=")) {
                final String v = res.substring(4);
                switchTab(0);
                edValue.setText(v);
                bg(res2 -> {
                    if (res2 != null && res2.startsWith("count")) {
                        infoText.setText("命中: " + v + " → " + res2.replace("count=", "") + " 个地址");
                        refineRow.setVisibility(View.VISIBLE);
                    }
                }, c2 -> {
                    JSONObject p = new JSONObject();
                    p.put("value", v);
                    p.put("type", t);
                    p.put("region", curRegion);
                    JSONObject r = c2.call(WireClient.CMD_SEARCH, p);
                    long count = r.optLong("count");
                    if (count >= 0 && count <= 200) loadResults(c2);
                    return "count=" + count;
                });
            }
        }, c -> {
            JSONObject p = new JSONObject();
            p.put("addr", "0x" + Long.toHexString(a));
            p.put("type", t);
            JSONObject r = c.call(WireClient.CMD_READ, p);
            return "val=" + r.optString("value");
        });
    }

    private void memSaveCurrent() {
        addSaveDialog("0x" + Long.toHexString(selAddr()), "");
    }

    /** 跳转到选中地址指向的指针 */
    private void memJumpPtr() {
        final long a = selAddr();
        bg(res -> {
            if (res != null && res.startsWith("ptr=")) {
                try {
                    long ptr = Long.decode(res.substring(4));
                    memLoad(ptr);
                } catch (Exception ignored) { toast("指针解析失败"); }
            }
        }, c -> {
            JSONObject p = new JSONObject();
            p.put("addr", "0x" + Long.toHexString(a));
            p.put("type", WireClient.VT_QWORD);
            JSONObject r = c.call(WireClient.CMD_READ, p);
            return "ptr=" + r.optString("value");
        });
    }

    class MemAdapter extends BaseAdapter {
        @Override public int getCount() { return memData == null ? 0 : (memData.length + 15) / 16; }
        @Override public Object getItem(int i) { return i; }
        @Override public long getItemId(int i) { return i; }

        @Override public View getView(int position, View convertView, ViewGroup parent) {
            View row = convertView;
            if (row == null) row = LayoutInflater.from(ctx).inflate(R.layout.row_mem, parent, false);
            TextView ta = row.findViewById(R.id.memRowAddr);
            TextView th = row.findViewById(R.id.memRowHex);
            TextView tas = row.findViewById(R.id.memRowAscii);
            View dot = row.findViewById(R.id.memBpDot);

            long rowAddr = memBase + position * 16L;
            ta.setText(String.format("%08x", rowAddr));

            StringBuilder hex = new StringBuilder();
            StringBuilder ascii = new StringBuilder();
            int start = position * 16;
            int end = Math.min(start + 16, memData.length);
            for (int i = start; i < end; i++) {
                if (i > start && (i - start) % 8 == 0) hex.append(' ');
                hex.append(String.format("%02x ", memData[i] & 0xFF));
                int b = memData[i] & 0xFF;
                ascii.append(b >= 32 && b < 127 ? (char) b : '.');
            }
            th.setText(hex.toString());
            tas.setText(ascii.toString());

            dot.setVisibility(bpAddrs.contains("0x" + Long.toHexString(rowAddr)) ? View.VISIBLE : View.GONE);
            dot.setOnClickListener(v -> showHitHistoryDialog());

            row.setBackgroundResource(R.drawable.mem_row_bg);
            row.setSelected(position == selRow);
            return row;
        }
    }

    /** 断点命中历史（点击内存页红点查看） */
    private void showHitHistoryDialog() {
        if (hitTitles.isEmpty()) { toast("暂无断点命中记录"); return; }
        ListView lv = new ListView(ctx);
        lv.setBackgroundColor(0xFF263238);
        lv.setDivider(null);
        lv.setAdapter(new BaseAdapter() {
            @Override public int getCount() { return hitTitles.size(); }
            @Override public Object getItem(int i) { return hitTitles.get(i); }
            @Override public long getItemId(int i) { return i; }
            @Override public View getView(int i, View c, ViewGroup p) {
                View row = c;
                if (row == null) row = LayoutInflater.from(ctx).inflate(R.layout.row_hit, p, false);
                ((TextView) row.findViewById(R.id.hitTitle)).setText(hitTitles.get(i));
                ((TextView) row.findViewById(R.id.hitBody)).setText(hitBodies.get(i));
                return row;
            }
        });
        new OverlayDialogBuilder(ctx)
                .setTitle("断点命中历史（" + hitCount + "）")
                .setView(lv)
                .setNegativeButton("关闭", null)
                .show();
    }

    // ================= 保存面板（GG 保存列表） =================

    private void initSavePanel() {
        saveList = panelSave.findViewById(R.id.saveList);
        saveAdapter = new SaveAdapter();
        saveList.setAdapter(saveAdapter);
        saveList.setOnItemClickListener((parent, view, position, id) -> {
            if (position < saves.size()) editSave(position);
        });
        saveList.setOnItemLongClickListener((parent, view, position, id) -> {
            if (position < saves.size()) saveMenu(position);
            return true;
        });
        panelSave.findViewById(R.id.btnSaveAdd).setOnClickListener(v -> addSaveDialog("", ""));
        panelSave.findViewById(R.id.btnSaveClear).setOnClickListener(v -> {
            new OverlayDialogBuilder(ctx)
                    .setTitle("清空保存列表？")
                    .setMessage("将删除全部 " + saves.size() + " 条保存项")
                    .setPositiveButton("清空", (d, w) -> {
                        saves.clear();
                        saveSelected.clear();
                        saveFrozen.clear();
                        saveSaves();
                    })
                    .setNegativeButton("取消", null).show();
        });
        panelSave.findViewById(R.id.btnSaveFreezeAll).setOnClickListener(v -> freezeAllSaves(true));
        panelSave.findViewById(R.id.btnSaveUnfreezeAll).setOnClickListener(v -> freezeAllSaves(false));
        panelSave.findViewById(R.id.btnSaveMore).setOnClickListener(v -> saveMoreMenu());
    }

    private void addSaveDialog(final String addr, final String value) {
        final EditText etAddr = new EditText(ctx);
        etAddr.setHint("地址 (0x...)");
        etAddr.setText(addr);
        etAddr.setTextColor(0xFFE0E0E0);
        final EditText etDesc = new EditText(ctx);
        etDesc.setHint("描述（如: 当前子弹）");
        etDesc.setTextColor(0xFFE0E0E0);
        final Spinner sp = new Spinner(ctx);
        ArrayAdapter<String> ad = new ArrayAdapter<>(ctx,
                android.R.layout.simple_spinner_dropdown_item, SAVE_TYPE_NAMES);
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(ad);
        sp.setSelection(0);
        LinearLayout ll = new LinearLayout(ctx);
        ll.setOrientation(LinearLayout.VERTICAL);
        ll.setPadding(30, 10, 30, 0);
        ll.addView(etAddr);
        ll.addView(etDesc);
        ll.addView(sp);
        new OverlayDialogBuilder(ctx)
                .setTitle("添加到保存")
                .setView(ll)
                .setPositiveButton("保存", (d, w) -> {
                    final String a = etAddr.getText().toString().trim();
                    final String desc = etDesc.getText().toString().trim();
                    if (a.isEmpty()) { toast("填写地址"); return; }
                    final String typ = SAVE_TYPE_NAMES[sp.getSelectedItemPosition()];
                    addSave(a, desc.isEmpty() ? a : desc, typ, value);
                })
                .setNegativeButton("取消", null).show();
    }

    private void addSave(final String addr, final String desc, final String type, final String value) {
        try {
            JSONObject a = new JSONObject();
            a.put("tag", desc);
            a.put("addr", addr);
            a.put("type", type);
            a.put("value", value == null ? "" : value);
            a.put("local", true);
            a.put("updated", new SimpleDateFormat("MM-dd HH:mm", Locale.US).format(new Date()));
            upsertSave(a);
            saveSaves();
            showNotif(NOTIF_ANNOT, "✓ 已保存: " + desc, 2000, null);
        } catch (Exception e) { toast("err: " + e.getMessage()); }
    }

    private void editSave(final int index) {
        JSONObject a = saves.get(index);
        final String addr = a.optString("addr");
        final EditText val = new EditText(ctx);
        val.setHint("新值");
        val.setText(a.optString("value"));
        val.setTextColor(0xFFE0E0E0);
        LinearLayout ll = new LinearLayout(ctx);
        ll.setOrientation(LinearLayout.VERTICAL);
        ll.setPadding(30, 10, 30, 0);
        ll.addView(val);
        new OverlayDialogBuilder(ctx)
                .setTitle("修改 " + a.optString("tag") + " (" + addr + ")")
                .setView(ll)
                .setPositiveButton("写入", (d, w) -> {
                    final String nv = val.getText().toString().trim();
                    if (nv.isEmpty()) return;
                    final int t = typeFromString(a.optString("type"));
                    bg(res -> {
                        try {
                            saves.get(index).put("value", nv);
                            saveSaves();
                        } catch (Exception ignored) {}
                    }, c -> {
                        JSONObject p = new JSONObject();
                        p.put("addr", addr);
                        p.put("value", nv);
                        p.put("type", t);
                        JSONObject r = c.call(WireClient.CMD_WRITE, p);
                        return "bytes=" + r.optLong("bytes");
                    });
                })
                .setNegativeButton("取消", null).show();
    }

    private void saveMenu(final int index) {
        JSONObject a = saves.get(index);
        final String addr = a.optString("addr");
        final String[] items = {"编辑描述", "修改值", "冻结/解冻", "跳转内存", "删除"};
        new OverlayDialogBuilder(ctx)
                .setTitle(a.optString("tag"))
                .setItems(items, (d, w) -> {
                    switch (w) {
                        case 0: {
                            final EditText desc = new EditText(ctx);
                            desc.setText(a.optString("tag"));
                            desc.setTextColor(0xFFE0E0E0);
                            new OverlayDialogBuilder(ctx)
                                    .setTitle("编辑描述")
                                    .setView(desc)
                                    .setPositiveButton("保存", (d2, w2) -> {
                                        try {
                                            saves.get(index).put("tag", desc.getText().toString().trim());
                                            saveSaves();
                                        } catch (Exception ignored) {}
                                    })
                                    .setNegativeButton("取消", null).show();
                            break;
                        }
                        case 1: editSave(index); break;
                        case 2: {
                            boolean on = !saveFrozen.contains(addr);
                            freezeSave(addr, a.optString("value"), typeFromString(a.optString("type")), on);
                            break;
                        }
                        case 3: {
                            try {
                                long aa = Long.decode(addr);
                                switchTab(1);
                                memLoad(aa);
                            } catch (Exception e) { toast("地址格式错误"); }
                            break;
                        }
                        case 4: {
                            saves.remove(index);
                            saveSelected.remove(addr);
                            saveFrozen.remove(addr);
                            saveSaves();
                            break;
                        }
                    }
                })
                .setNegativeButton("取消", null).show();
    }

    private void freezeAllSaves(final boolean on) {
        bg(res -> {
            saveFrozen.clear();
            if (on) {
                for (JSONObject a : saves) {
                    String addr = a.optString("addr");
                    if (!addr.isEmpty()) saveFrozen.add(addr);
                }
            }
            saveAdapter.notifyDataSetChanged();
            statusBarUpdate();
        }, c -> {
            int n = 0;
            for (JSONObject a : saves) {
                String addr = a.optString("addr");
                if (addr.isEmpty()) continue;
                JSONObject p = new JSONObject();
                if (on) {
                    String v = a.optString("value");
                    p.put("addr", addr);
                    p.put("value", v.isEmpty() ? "0" : v);
                    p.put("type", typeFromString(a.optString("type")));
                    c.call(WireClient.CMD_FREEZE, p);
                } else {
                    p.put("addr", addr);
                    c.call(WireClient.CMD_UNFREEZE, p);
                }
                n++;
            }
            return (on ? "frozen " : "unfrozen ") + n;
        });
    }

    private void freezeSave(final String addr, final String value, final int type, final boolean on) {
        bg(res -> {
            if (on) saveFrozen.add(addr); else saveFrozen.remove(addr);
            saveAdapter.notifyDataSetChanged();
            statusBarUpdate();
        }, c -> {
            JSONObject p = new JSONObject();
            if (on) {
                p.put("addr", addr);
                p.put("value", value == null || value.isEmpty() ? "0" : value);
                p.put("type", type);
                c.call(WireClient.CMD_FREEZE, p);
                return "frozen";
            } else {
                p.put("addr", addr);
                c.call(WireClient.CMD_UNFREEZE, p);
                return "unfrozen";
            }
        });
    }

    private void saveMoreMenu() {
        final String[] items = {"导出保存列表", "排序（按地址）", "排序（按名称）"};
        new OverlayDialogBuilder(ctx)
                .setTitle("更多")
                .setItems(items, (d, w) -> {
                    switch (w) {
                        case 0: exportSaves(); break;
                        case 1:
                            saves.sort((x, y) -> {
                                try { return Long.compare(Long.decode(x.optString("addr")), Long.decode(y.optString("addr"))); }
                                catch (Exception e) { return 0; }
                            });
                            saveSaves();
                            break;
                        case 2:
                            saves.sort((x, y) -> x.optString("tag").compareTo(y.optString("tag")));
                            saveSaves();
                            break;
                    }
                })
                .setNegativeButton("取消", null).show();
    }

    private void exportSaves() {
        StringBuilder sb = new StringBuilder();
        for (JSONObject a : saves) sb.append(a.toString()).append('\n');
        try {
            File dir = ctx.getExternalFilesDir(null);
            if (dir == null) dir = ctx.getFilesDir();
            File f = new File(dir, "ggmcp_saves_" + System.currentTimeMillis() + ".txt");
            FileOutputStream os = new FileOutputStream(f);
            os.write(sb.toString().getBytes("UTF-8"));
            os.close();
            toast("保存列表已导出: " + f.getAbsolutePath());
        } catch (Exception e) {
            toast("导出失败: " + e.getMessage());
        }
    }

    /** PC 端 gg_annotate_* 经 agentd 中继的 annotate 事件 */
    private void handleAnnotEvent(JSONObject d) {
        String op = d.optString("op", "add");
        JSONObject ann = d.optJSONObject("ann");
        if (ann == null) ann = d;
        if ("clear".equals(op)) {
            saves.clear();
        } else if ("remove".equals(op)) {
            String tag = ann.optString("tag");
            for (int i = saves.size() - 1; i >= 0; i--) {
                if (saves.get(i).optString("tag").equals(tag)) saves.remove(i);
            }
        } else {
            upsertSave(ann);
            String tag = ann.optString("tag");
            showNotif(NOTIF_ANNOT, "✓ 已保存: " + (tag.isEmpty() ? ann.optString("addr") : tag), 2000, null);
        }
        saveSaves();
    }

    private void upsertSave(JSONObject a) {
        String tag = a.optString("tag");
        for (int i = 0; i < saves.size(); i++) {
            if (saves.get(i).optString("tag").equals(tag)) {
                saves.set(i, a);
                saveAdapter.notifyDataSetChanged();
                statusBarUpdate();
                return;
            }
        }
        saves.add(0, a);
        saveAdapter.notifyDataSetChanged();
        statusBarUpdate();
    }

    private void saveSaves() {
        JSONArray arr = new JSONArray();
        for (JSONObject a : saves) arr.put(a);
        prefs.edit().putString("annots", arr.toString()).apply();
        saveAdapter.notifyDataSetChanged();
        statusBarUpdate();
    }

    private void loadSaves() {
        try {
            JSONArray arr = new JSONArray(prefs.getString("annots", "[]"));
            saves.clear();
            for (int i = 0; i < arr.length(); i++) saves.add(arr.optJSONObject(i));
        } catch (Exception ignored) {}
        saveAdapter.notifyDataSetChanged();
        statusBarUpdate();
    }

    private int typeFromString(String t) {
        if (t == null) return WireClient.VT_DWORD;
        String s = t.trim().toLowerCase(Locale.US);
        if (s.startsWith("byte") || s.equals("by")) return WireClient.VT_BYTE;
        if (s.startsWith("word") || s.equals("wo") || s.startsWith("int16")) return WireClient.VT_WORD;
        if (s.startsWith("dword") || s.equals("dw") || s.startsWith("int") || s.startsWith("uint")
                || s.startsWith("ptr") || s.startsWith("pointer")) return WireClient.VT_DWORD;
        if (s.startsWith("qword") || s.equals("qw") || s.startsWith("long")) return WireClient.VT_QWORD;
        if (s.startsWith("float") || s.equals("fl")) return WireClient.VT_FLOAT;
        if (s.startsWith("double") || s.equals("db")) return WireClient.VT_DOUBLE;
        return WireClient.VT_DWORD;
    }

    private String typeAbbr(int t) {
        switch (t) {
            case WireClient.VT_BYTE: return "BY";
            case WireClient.VT_WORD: return "WO";
            case WireClient.VT_QWORD: return "QW";
            case WireClient.VT_FLOAT: return "FL";
            case WireClient.VT_DOUBLE: return "DB";
            default: return "DW";
        }
    }

    class SaveAdapter extends BaseAdapter {
        @Override public int getCount() { return saves.size(); }
        @Override public Object getItem(int i) { return saves.get(i); }
        @Override public long getItemId(int i) { return i; }

        @Override public View getView(int position, View convertView, ViewGroup parent) {
            View row = convertView;
            if (row == null) row = LayoutInflater.from(ctx).inflate(R.layout.row_annot, parent, false);
            JSONObject a = saves.get(position);
            final String addr = a.optString("addr");
            String desc = a.optString("tag");
            if (desc.isEmpty()) desc = addr;
            CheckBox cs = row.findViewById(R.id.chkSel);
            TextView td = row.findViewById(R.id.txtDesc);
            TextView ta = row.findViewById(R.id.txtAddr);
            TextView tv = row.findViewById(R.id.txtValue);
            TextView tt = row.findViewById(R.id.txtType);
            CheckBox cf = row.findViewById(R.id.chkFreeze);

            td.setText(desc);
            ta.setText(addr.replace("0x", ""));
            tv.setText(a.optString("value"));
            tt.setText(typeAbbr(typeFromString(a.optString("type"))));

            cs.setOnCheckedChangeListener(null);
            cs.setChecked(saveSelected.contains(addr));
            cs.setOnCheckedChangeListener((b, c) -> {
                if (c) saveSelected.add(addr); else saveSelected.remove(addr);
                statusBarUpdate();
            });

            cf.setOnCheckedChangeListener(null);
            cf.setChecked(saveFrozen.contains(addr));
            cf.setOnCheckedChangeListener((b, c) -> freezeSave(addr, a.optString("value"), typeFromString(a.optString("type")), c));

            tv.setTextColor(saveFrozen.contains(addr) ? 0xFF4CAF50 : 0xFFFFFFFF);
            return row;
        }
    }

    // ================= 结果适配器 =================

    class ResultAdapter extends BaseAdapter {
        final ArrayList<String> addrs = new ArrayList<>();
        final ArrayList<String> values = new ArrayList<>();

        void setData(ArrayList<String> a, ArrayList<String> v) {
            addrs.clear(); addrs.addAll(a);
            values.clear(); values.addAll(v);
            notifyDataSetChanged();
        }

        void clearData() {
            addrs.clear(); values.clear();
            notifyDataSetChanged();
        }

        void removeAt(int i) {
            if (i < addrs.size()) addrs.remove(i);
            if (i < values.size()) values.remove(i);
            notifyDataSetChanged();
        }

        @Override public int getCount() { return addrs.size(); }
        @Override public Object getItem(int i) { return addrs.get(i); }
        @Override public long getItemId(int i) { return i; }

        @Override public View getView(int position, View convertView, ViewGroup parent) {
            View row = convertView;
            if (row == null) row = LayoutInflater.from(ctx).inflate(R.layout.row_result, parent, false);
            final String addr = addrs.get(position);
            CheckBox cs = row.findViewById(R.id.chkSelect);
            TextView ta = row.findViewById(R.id.txtAddr);
            TextView tv = row.findViewById(R.id.txtValue);
            TextView tr = row.findViewById(R.id.txtRegion);
            CheckBox cf = row.findViewById(R.id.chkFreeze);

            ta.setText(addr.replace("0x", ""));
            tv.setText(values.get(position));
            tr.setText(regionOf(addr));

            cs.setOnCheckedChangeListener(null);
            cs.setChecked(resultSelected.contains(addr));
            cs.setOnCheckedChangeListener((b, c) -> {
                if (c) resultSelected.add(addr); else resultSelected.remove(addr);
                statusBarUpdate();
            });

            cf.setOnCheckedChangeListener(null);
            cf.setChecked(resultFrozen.contains(addr));
            final String value = values.get(position);
            cf.setOnCheckedChangeListener((b, c) -> freezeResult(addr, value, c));
            return row;
        }
    }

    private String regionOf(String addr) {
        long a;
        try {
            a = Long.decode(addr);
        } catch (Exception e) {
            try { a = Long.parseLong(addr, 16); } catch (Exception e2) { return "anon"; }
        }
        for (int i = 0; i < regionRanges.size(); i++) {
            long[] r = regionRanges.get(i);
            if (a >= r[0] && a < r[1]) return regionTags.get(i);
        }
        return "anon";
    }

    private void loadRegions() {
        bg(res -> {}, c -> {
            JSONObject r = c.call(WireClient.CMD_TARGET_INFO, new JSONObject());
            JSONArray regs = r.optJSONArray("regions");
            final ArrayList<long[]> nr = new ArrayList<>();
            final ArrayList<String> nt = new ArrayList<>();
            if (regs != null) {
                for (int i = 0; i < regs.length(); i++) {
                    JSONObject o = regs.optJSONObject(i);
                    if (o == null) continue;
                    long s = Long.decode(o.optString("start", "0x0"));
                    long e = Long.decode(o.optString("end", "0x0"));
                    String path = o.optString("path", "");
                    String tag;
                    if (path.isEmpty()) tag = "anon";
                    else if (path.contains("[heap]")) tag = "heap";
                    else if (path.contains("[stack]")) tag = "stack";
                    else tag = "code";
                    nr.add(new long[]{s, e});
                    nt.add(tag);
                }
            }
            h.post(() -> {
                regionRanges.clear(); regionRanges.addAll(nr);
                regionTags.clear(); regionTags.addAll(nt);
                if (resultAdapter != null) resultAdapter.notifyDataSetChanged();
            });
            return "regions=" + nr.size();
        });
    }

    // ================= 设置面板 =================

    private void initSettingsPanel() {
        statusKernel = panelSettings.findViewById(R.id.statusKernel);
        tvProcessName = panelSettings.findViewById(R.id.tvProcessName);
        seekOpacity = panelSettings.findViewById(R.id.seekOpacity);
        tvOpacity = panelSettings.findViewById(R.id.tvOpacity);
        swVibrate = panelSettings.findViewById(R.id.swVibrate);
        swNotify = panelSettings.findViewById(R.id.swNotify);
        swBoot = panelSettings.findViewById(R.id.swBoot);

        panelSettings.findViewById(R.id.btnRefreshStatus).setOnClickListener(v -> refreshStatus());
        panelSettings.findViewById(R.id.btnEngine).setOnClickListener(v -> {
            toast("启动引擎…");
            new Thread(() -> {
                final String msg = EngineManager.ensureEngine(ctx);
                h.post(() -> { toast(msg); refreshStatus(); });
            }).start();
        });
        panelSettings.findViewById(R.id.btnSelectProcess).setOnClickListener(v -> showProcessDialog());
        panelSettings.findViewById(R.id.btnDetach).setOnClickListener(v -> {
            bg(res -> {
                sessionPid = -1;
                sessionName = "";
                titleText.setText("未选择进程");
                tvProcessName.setText("未选择进程");
            }, c -> {
                c.call(WireClient.CMD_DETACH, new JSONObject());
                return "ok";
            });
        });
        panelSettings.findViewById(R.id.btnLog).setOnClickListener(v -> showLogDialog());
        panelSettings.findViewById(R.id.btnExportSave).setOnClickListener(v -> exportSaves());
        panelSettings.findViewById(R.id.btnAbout).setOnClickListener(v ->
                new OverlayDialogBuilder(ctx)
                        .setTitle("关于")
                        .setMessage("GG-MCP 悬浮监控 v2.0\nGG 风格 4 标签：搜索 / 内存 / 保存 / 设置\n"
                                + "断点、日志、进程合并到对应页面\n仅供技术研究学习")
                        .setPositiveButton("OK", null).show());

        // 读回设置
        swVibrate.setChecked(prefs.getBoolean("vibrate", true));
        swNotify.setChecked(prefs.getBoolean("notify_bp", true));
        swBoot.setChecked(prefs.getBoolean("boot", false));
        int opacity = prefs.getInt("opacity", 85);
        seekOpacity.setProgress(opacity);
        tvOpacity.setText(opacity + "%");
        applyOpacity(opacity);
        int ballSize = prefs.getInt("ball_size", 54);
        int fontIdx = prefs.getInt("font_scale", 1);
        applyFontScale(fontIdx == 0 ? 0.9f : fontIdx == 2 ? 1.15f : 1.0f);
        markChoice(panelSettings.findViewById(R.id.ballSizeS), panelSettings.findViewById(R.id.ballSizeM),
                panelSettings.findViewById(R.id.ballSizeL), ballSize == 40 ? 0 : ballSize == 70 ? 2 : 1);
        markChoice(panelSettings.findViewById(R.id.fontS), panelSettings.findViewById(R.id.fontM),
                panelSettings.findViewById(R.id.fontL), fontIdx);

        swVibrate.setOnCheckedChangeListener((b, c) -> prefs.edit().putBoolean("vibrate", c).apply());
        swNotify.setOnCheckedChangeListener((b, c) -> prefs.edit().putBoolean("notify_bp", c).apply());
        swBoot.setOnCheckedChangeListener((b, c) -> prefs.edit().putBoolean("boot", c).apply());

        seekOpacity.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar sb, int p, boolean fromUser) {
                tvOpacity.setText(p + "%");
                if (fromUser) {
                    prefs.edit().putInt("opacity", p).apply();
                    applyOpacity(p);
                }
            }
            @Override public void onStartTrackingTouch(SeekBar sb) {}
            @Override public void onStopTrackingTouch(SeekBar sb) {}
        });

        panelSettings.findViewById(R.id.ballSizeS).setOnClickListener(v -> setBallSize(40, 0));
        panelSettings.findViewById(R.id.ballSizeM).setOnClickListener(v -> setBallSize(54, 1));
        panelSettings.findViewById(R.id.ballSizeL).setOnClickListener(v -> setBallSize(70, 2));
        panelSettings.findViewById(R.id.fontS).setOnClickListener(v -> setFontSize(0.9f, 0));
        panelSettings.findViewById(R.id.fontM).setOnClickListener(v -> setFontSize(1.0f, 1));
        panelSettings.findViewById(R.id.fontL).setOnClickListener(v -> setFontSize(1.15f, 2));

        tvProcessName.setText(sessionPid > 0 ? sessionName : "未选择进程");
    }

    private void markChoice(TextView a, TextView b, TextView c, int idx) {
        TextView[] arr = {a, b, c};
        for (int i = 0; i < 3; i++) {
            arr[i].setTextColor(i == idx ? 0xFF101010 : 0xFFB0B0B0);
            arr[i].setBackgroundResource(i == idx ? R.drawable.btn_primary : R.drawable.btn_dark);
        }
    }

    private void setBallSize(int dp, int idx) {
        prefs.edit().putInt("ball_size", dp).apply();
        markChoice(panelSettings.findViewById(R.id.ballSizeS), panelSettings.findViewById(R.id.ballSizeM),
                panelSettings.findViewById(R.id.ballSizeL), idx);
        if (settingsListener != null) settingsListener.onBallSizeChanged(dp);
    }

    private void setFontSize(float scale, int idx) {
        prefs.edit().putInt("font_scale", idx).apply();
        markChoice(panelSettings.findViewById(R.id.fontS), panelSettings.findViewById(R.id.fontM),
                panelSettings.findViewById(R.id.fontL), idx);
        applyFontScale(scale);
        if (settingsListener != null) settingsListener.onFontScaleChanged(scale);
    }

    private void applyFontScale(float scale) {
        fontScale = scale;
        walkFont(root);
    }

    private void walkFont(View v) {
        if (v instanceof TextView) {
            TextView tv = (TextView) v;
            Float base = baseFontSizes.get(tv);
            if (base == null) {
                base = tv.getTextSize() / ctx.getResources().getDisplayMetrics().density;
                baseFontSizes.put(tv, base);
            }
            tv.setTextSize(base * fontScale);
        }
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) walkFont(g.getChildAt(i));
        }
    }

    private void applyOpacity(int percent) {
        if (root.getBackground() != null) {
            root.getBackground().setAlpha(percent * 255 / 100);
        }
    }

    private void refreshStatus() {
        bg(res -> {
            if (res != null && !res.startsWith("err")) statusKernel.setText(res);
        }, c -> {
            JSONObject r = c.call(WireClient.CMD_KERNEL_STATUS, new JSONObject());
            boolean ko = r.optBoolean("ko_loaded");
            if (ko) return "QX 已加载 (" + r.optString("state_str") + ")";
            return r.optString("state_str", "未知") + " (KO 未加载)";
        });
    }

    // ================= 选择进程对话框 =================

    private void showProcessDialog() {
        View v = LayoutInflater.from(ctx).inflate(R.layout.dialog_process, null);
        filterText = v.findViewById(R.id.filterText);
        processList = v.findViewById(R.id.processList);
        sessionText = v.findViewById(R.id.sessionText);
        v.findViewById(R.id.btnProcRefresh).setOnClickListener(x -> loadProcesses());
        // 两行进程行：应用标签 + 包名 · PID（应用优先排序见 loadProcesses）
        processList.setAdapter(new BaseAdapter() {
            @Override public int getCount() { return procRows.size(); }
            @Override public Object getItem(int i) { return procRows.get(i); }
            @Override public long getItemId(int i) { return i; }
            @Override public View getView(int i, View c, ViewGroup p) {
                View row = c;
                if (row == null) row = LayoutInflater.from(ctx).inflate(R.layout.row_process, p, false);
                String[] parts = procRows.get(i).split("\u0001", 2);
                ((TextView) row.findViewById(R.id.procLabel)).setText(parts[0]);
                ((TextView) row.findViewById(R.id.procMeta)).setText(parts.length > 1 ? parts[1] : "");
                return row;
            }
        });
        processList.setOnItemClickListener((parent, view, position, id) -> {
            if (position < procPids.size()) attach(procPids.get(position));
        });
        filterText.addTextChangedListener(new android.text.TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) { loadProcesses(); }
            @Override public void afterTextChanged(android.text.Editable s) {}
        });
        procDialog = new OverlayDialogBuilder(ctx)
                .setTitle("选择进程（应用优先 · 点击附加）")
                .setView(v)
                .setNegativeButton("取消", null)
                .create();
        try {
            if (procDialog.getWindow() != null) {
                procDialog.getWindow().setType(WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY);
            }
            procDialog.show();
        } catch (Exception e) {
            android.util.Log.e("GGMCP", "process dialog failed: " + e.getMessage());
        }
        loadProcesses();
    }

    private void loadProcesses() {
        final String filter = filterText != null ? filterText.getText().toString() : "";
        bg(res -> {}, c -> {
            JSONObject p = new JSONObject();
            if (!filter.isEmpty()) p.put("filter", filter);
            JSONObject r = c.call(WireClient.CMD_LIST_PROCESSES, p);
            JSONArray arr = r.optJSONArray("processes");
            final android.content.pm.PackageManager pm = ctx.getPackageManager();
            final ArrayList<String> apps = new ArrayList<>();    // 应用（包名类）
            final ArrayList<Integer> appPids = new ArrayList<>();
            final ArrayList<String> sys = new ArrayList<>();     // 系统进程
            final ArrayList<Integer> sysPids = new ArrayList<>();
            if (arr != null) {
                for (int i = 0; i < arr.length(); i++) {
                    JSONObject o = arr.optJSONObject(i);
                    if (o == null) continue;
                    int pid = o.optInt("pid");
                    String name = o.optString("name");
                    String comm = o.optString("comm");
                    if (name.startsWith("[")) continue;          // 内核线程过滤
                    if (name.contains(".")) {
                        // 应用：解析中文标签（去掉 :remote/:push 等进程后缀再查包）
                        String label = name;
                        try {
                            String base = name;
                            int ci = base.indexOf(':');
                            if (ci > 0) base = base.substring(0, ci);
                            android.content.pm.ApplicationInfo ai = pm.getApplicationInfo(base, 0);
                            if (ai != null) {
                                CharSequence l = ai.loadLabel(pm);
                                if (l != null && l.length() > 0) label = l.toString();
                            }
                        } catch (Exception ignored) {}
                        apps.add(label + "\u0001" + name + " · PID " + pid);
                        appPids.add(pid);
                    } else {
                        sys.add(name + "\u0001" + "PID " + pid + (comm.isEmpty() ? "" : " · " + comm));
                        sysPids.add(pid);
                    }
                }
            }
            h.post(() -> {
                procRows.clear(); procRows.addAll(apps); procRows.addAll(sys);
                procPids.clear(); procPids.addAll(appPids); procPids.addAll(sysPids);
                if (processList != null && processList.getAdapter() != null) {
                    ((BaseAdapter) processList.getAdapter()).notifyDataSetChanged();
                }
                if (sessionText != null) {
                    sessionText.setText("应用 " + appPids.size() + " · 系统 " + sysPids.size()
                            + "（输入包名关键字过滤）");
                }
            });
            return "n=" + (appPids.size() + sysPids.size());
        });
    }

    private void attach(final int pid) {
        bg(res -> {
            if (procDialog != null && procDialog.isShowing()) procDialog.dismiss();
            showNotif(NOTIF_CMD, "已附加 pid " + pid, 2000, null);
            loadRegions();
        }, c -> {
            JSONObject p = new JSONObject();
            p.put("pid", pid);
            JSONObject r = c.call(WireClient.CMD_ATTACH, p);
            return "ok: " + r.optString("name");
        });
    }

    // ================= 操作日志对话框 =================

    private void showLogDialog() {
        View v = LayoutInflater.from(ctx).inflate(R.layout.dialog_log, null);
        logList = v.findViewById(R.id.logList);
        filterAll = v.findViewById(R.id.filterAll);
        filterCmd = v.findViewById(R.id.filterCmd);
        filterBp = v.findViewById(R.id.filterBp);
        filterAnnot = v.findViewById(R.id.filterAnnot);
        if (logAdapter == null) logAdapter = new LogAdapter();
        logList.setAdapter(logAdapter);
        CompoundButton.OnCheckedChangeListener fl = (b, c) -> {
            filterAll.setChecked(false);
            logAdapter.applyFilter();
        };
        filterCmd.setOnCheckedChangeListener(fl);
        filterBp.setOnCheckedChangeListener(fl);
        filterAnnot.setOnCheckedChangeListener(fl);
        filterAll.setOnCheckedChangeListener((b, c) -> {
            if (c) {
                filterCmd.setChecked(true);
                filterBp.setChecked(true);
                filterAnnot.setChecked(true);
                logAdapter.applyFilter();
            }
        });
        v.findViewById(R.id.btnExportLog).setOnClickListener(x -> exportLog());
        v.findViewById(R.id.btnClearLog).setOnClickListener(x -> {
            logAdapter.clear();
            toast("日志已清空");
        });
        new OverlayDialogBuilder(ctx)
                .setTitle("操作日志")
                .setView(v)
                .setNegativeButton("关闭", null)
                .show();
        logAdapter.applyFilter();
    }

    private void exportLog() {
        if (logAdapter == null) return;
        StringBuilder sb = new StringBuilder();
        for (LogItem it : logAdapter.all) {
            sb.append(it.time).append(' ').append(it.title).append(' ').append(it.body).append('\n');
        }
        try {
            File dir = ctx.getExternalFilesDir(null);
            if (dir == null) dir = ctx.getFilesDir();
            File f = new File(dir, "ggmcp_log_" + System.currentTimeMillis() + ".txt");
            FileOutputStream os = new FileOutputStream(f);
            os.write(sb.toString().getBytes("UTF-8"));
            os.close();
            toast("日志已导出: " + f.getAbsolutePath());
        } catch (Exception e) {
            toast("导出失败: " + e.getMessage());
        }
    }

    static class LogItem {
        final String time, title, body;
        final int kind;
        final boolean ok, err;
        LogItem(String time, String title, String body, int kind, boolean ok, boolean err) {
            this.time = time; this.title = title; this.body = body;
            this.kind = kind; this.ok = ok; this.err = err;
        }
    }

    class LogAdapter extends BaseAdapter {
        final ArrayList<LogItem> all = new ArrayList<>();
        final ArrayList<LogItem> shown = new ArrayList<>();

        void add(LogItem it) {
            all.add(it);
            if (all.size() > 500) all.remove(0);
            applyFilter();
        }

        void clear() {
            all.clear();
            shown.clear();
            notifyDataSetChanged();
        }

        void applyFilter() {
            shown.clear();
            boolean fA = filterAll != null && filterAll.isChecked();
            boolean fC = filterCmd != null && filterCmd.isChecked();
            boolean fB = filterBp != null && filterBp.isChecked();
            boolean fN = filterAnnot != null && filterAnnot.isChecked();
            if (filterAll == null) { fA = true; fC = fB = fN = true; }
            for (LogItem it : all) {
                if (fA) { shown.add(it); continue; }
                if (it.kind == LOG_BP && fB) { shown.add(it); continue; }
                if (it.kind == LOG_ANNOT && fN) { shown.add(it); continue; }
                if (it.kind == LOG_CMD && fC) { shown.add(it); continue; }
            }
            notifyDataSetChanged();
            if (logList != null && shown.size() > 0) {
                logList.post(() -> {
                    if (logList != null && shown.size() > 0) logList.setSelection(shown.size() - 1);
                });
            }
        }

        @Override public int getCount() { return shown.size(); }
        @Override public Object getItem(int i) { return shown.get(i); }
        @Override public long getItemId(int i) { return i; }

        @Override public View getView(int position, View convertView, ViewGroup parent) {
            View row = convertView;
            if (row == null) row = LayoutInflater.from(ctx).inflate(R.layout.row_log, parent, false);
            LogItem it = shown.get(position);
            TextView t = row.findViewById(R.id.logTime);
            TextView ti = row.findViewById(R.id.logTitle);
            TextView b = row.findViewById(R.id.logBody);
            t.setText(it.time);
            ti.setText(it.title);
            b.setText(it.body);
            switch (it.kind) {
                case LOG_BP:
                    row.setBackgroundColor(0xFF2A1418);
                    ti.setTextColor(0xFFFF7A7A);
                    b.setTextColor(0xFFE8B0B0);
                    break;
                case LOG_ANNOT:
                    row.setBackgroundColor(0xFF14241A);
                    ti.setTextColor(0xFF7FD88A);
                    b.setTextColor(0xFFC8E8D0);
                    break;
                default:
                    row.setBackgroundColor(0xFF263238);
                    if (it.err) { ti.setTextColor(0xFFFF6B6B); b.setTextColor(0xFFFFA0A0); }
                    else if (it.ok) { ti.setTextColor(0xFF7FD88A); b.setTextColor(0xFFD8D8E0); }
                    else { ti.setTextColor(0xFF9A9AA5); b.setTextColor(0xFFD8D8E0); }
            }
            return row;
        }
    }

    private void toast(String s) { Toast.makeText(ctx, s, Toast.LENGTH_SHORT).show(); }

    /**
     * 悬浮窗服务没有 Activity token，普通 AlertDialog 会 BadTokenException。
     * 重写 show()：以 TYPE_APPLICATION_OVERLAY 窗口类型弹出，悬浮于任何界面之上。
     */
    private static class OverlayDialogBuilder extends AlertDialog.Builder {
        OverlayDialogBuilder(Context c) { super(c); }
        @Override public AlertDialog show() {
            AlertDialog d = create();
            try {
                if (d.getWindow() != null) {
                    d.getWindow().setType(WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY);
                }
                d.show();
            } catch (Exception e) {
                android.util.Log.e("GGMCP", "dialog show failed: " + e.getMessage());
            }
            return d;
        }
    }

    private int dp(int v) { return Math.round(v * ctx.getResources().getDisplayMetrics().density); }
}
