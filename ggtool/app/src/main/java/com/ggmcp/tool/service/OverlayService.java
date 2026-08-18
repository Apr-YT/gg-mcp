package com.ggmcp.tool.service;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import com.ggmcp.tool.MainActivity;
import com.ggmcp.tool.R;
import com.ggmcp.tool.mock.DataStore;
import com.ggmcp.tool.mock.MockMemoryRepository;
import com.ggmcp.tool.model.FreezeItem;
import com.ggmcp.tool.model.SearchResult;
import com.ggmcp.tool.model.SavedItem;
import com.ggmcp.tool.model.ValueType;
import com.ggmcp.tool.ui.editor.EditValueDialog;
import com.ggmcp.tool.ui.freeze.FreezeAdapter;
import com.ggmcp.tool.ui.keyboard.NumericKeyboard;
import com.ggmcp.tool.ui.region.RegionDialog;
import com.ggmcp.tool.ui.saved.SavedAdapter;
import com.ggmcp.tool.ui.search.SearchAdapter;
import com.ggmcp.tool.util.Prefs;
import com.ggmcp.tool.util.UiUtil;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * 悬浮窗前台服务：悬浮球（WindowManager 直挂）+ 主面板（Overlay 窗口）。
 * 第一阶段全部 Mock 数据，不实现真实内存读写。
 */
public class OverlayService extends Service {

    private static final String CHANNEL_ID = "gg_tool_overlay";
    private static final int NOTIF_ID = 1;

    private static volatile boolean running = false;

    public static boolean isRunning() {
        return running;
    }

    private WindowManager wm;
    private View ballView;
    private WindowManager.LayoutParams ballParams;
    private boolean ballDragging = false;
    private float downRawX, downRawY;
    private int downX, downY;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private boolean panelVisible = false;
    private View panelView;
    private WindowManager.LayoutParams panelParams;

    private final MockMemoryRepository mockRepo = new MockMemoryRepository();
    private List<SearchResult> results = new ArrayList<>();
    private ValueType currentType = ValueType.DWORD;
    private int currentMode = 0;
    private String currentRange = "";
    private float speed = 1.0f;
    private String lastSearchValue = "";

    private SearchAdapter searchAdapter;
    private SavedAdapter savedAdapter;
    private FreezeAdapter freezeAdapter;

    private LinearLayout panelSearch;
    private LinearLayout panelSaved;
    private LinearLayout panelFreeze;
    private TextView tvResultCount;

    private final DataStore dataStore = DataStore.get();

    @Override
    public void onCreate() {
        super.onCreate();
        running = true;
        wm = (WindowManager) getSystemService(WINDOW_SERVICE);
        startForegroundCompat();
        showBall();
        dataStore.addListener(dataListener);
    }

    private final DataStore.Listener dataListener = new DataStore.Listener() {
        @Override
        public void onSavedChanged() {
            handler.post(new Runnable() {
                @Override
                public void run() {
                    if (savedAdapter != null) savedAdapter.notifyDataSetChanged();
                }
            });
        }

        @Override
        public void onFrozenChanged() {
            handler.post(new Runnable() {
                @Override
                public void run() {
                    if (freezeAdapter != null) freezeAdapter.notifyDataSetChanged();
                }
            });
        }
    };

    private void startForegroundCompat() {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel ch = new NotificationChannel(CHANNEL_ID, "GG Tool Overlay",
                    NotificationManager.IMPORTANCE_LOW);
            nm.createNotificationChannel(ch);
        }
        Intent i = new Intent(this, MainActivity.class);
        PendingIntent pi = PendingIntent.getActivity(this, 0, i,
                PendingIntent.FLAG_IMMUTABLE);
        Notification.Builder b = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);
        Notification n = b.setContentTitle(getString(R.string.notif_title))
                .setContentText(getString(R.string.notif_text))
                .setSmallIcon(R.drawable.ic_launcher_foreground)
                .setContentIntent(pi)
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(NOTIF_ID, n);
        }
    }

    // ==================== 悬浮球 ====================

    private void showBall() {
        ballView = LayoutInflater.from(this).inflate(R.layout.view_ball, null);
        ballParams = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT,
                Build.VERSION.SDK_INT >= 26
                        ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                        : WindowManager.LayoutParams.TYPE_PHONE,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.TRANSLUCENT);
        ballParams.gravity = Gravity.TOP | Gravity.START;
        ballParams.x = Prefs.getBallX(this);
        ballParams.y = Prefs.getBallY(this);
        ballParams.alpha = Prefs.getBallAlpha(this);

        // GG 悬浮球：中心图标点击切换面板；两侧箭头调 speedhack 显示
        final TextView tvSpeed = ballView.findViewById(R.id.tv_speed);
        ballView.findViewById(R.id.btn_ball).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                togglePanel();
            }
        });
        ballView.findViewById(R.id.btn_speed_dec).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                speed = Math.max(0.1f, speed - 0.1f);
                if (tvSpeed != null) tvSpeed.setText(String.format("%.1fx", speed));
            }
        });
        ballView.findViewById(R.id.btn_speed_inc).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                speed = Math.min(10f, speed + 0.1f);
                if (tvSpeed != null) tvSpeed.setText(String.format("%.1fx", speed));
            }
        });

        ballView.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent e) {
                switch (e.getAction()) {
                    case MotionEvent.ACTION_DOWN:
                        ballDragging = false;
                        downRawX = e.getRawX();
                        downRawY = e.getRawY();
                        downX = ballParams.x;
                        downY = ballParams.y;
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        float dx = e.getRawX() - downRawX;
                        float dy = e.getRawY() - downRawY;
                        if (Math.abs(dx) > UiUtil.dp(OverlayService.this, 8)
                                || Math.abs(dy) > UiUtil.dp(OverlayService.this, 8)) {
                            ballDragging = true;
                        }
                        if (ballDragging) {
                            ballParams.x = downX + (int) dx;
                            ballParams.y = downY + (int) dy;
                            wm.updateViewLayout(ballView, ballParams);
                        }
                        return true;
                    case MotionEvent.ACTION_UP:
                        if (ballDragging) {
                            Prefs.saveBallPos(OverlayService.this, ballParams.x, ballParams.y);
                        }
                        return true;
                    default:
                        return false;
                }
            }
        });
        wm.addView(ballView, ballParams);
    }

    private void removeBall() {
        if (ballView != null) {
            try {
                wm.removeView(ballView);
            } catch (Exception ignored) {
            }
            ballView = null;
        }
    }

    // ==================== 主面板 ====================

    private void togglePanel() {
        if (panelVisible) {
            hidePanel();
        } else {
            showPanel();
        }
    }

    private void showPanel() {
        if (panelVisible) return;
        panelView = LayoutInflater.from(this).inflate(R.layout.panel_main, null);
        panelParams = new WindowManager.LayoutParams(
                (int) (getResources().getDisplayMetrics().widthPixels * 0.92f),
                (int) (getResources().getDisplayMetrics().heightPixels * 0.82f),
                Build.VERSION.SDK_INT >= 26
                        ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                        : WindowManager.LayoutParams.TYPE_PHONE,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.TRANSLUCENT);
        panelParams.gravity = Gravity.CENTER;
        panelParams.alpha = 1.0f;

        initPanelUi(panelView);
        wm.addView(panelView, panelParams);
        panelVisible = true;
    }

    private void hidePanel() {
        if (!panelVisible) return;
        try {
            wm.removeView(panelView);
        } catch (Exception ignored) {
        }
        panelVisible = false;
        panelView = null;
    }

    private void initPanelUi(final View panel) {
        // 标题栏拖动
        final View titleBar = panel.findViewById(R.id.title_bar);
        final WindowManager.LayoutParams lp = panelParams;
        titleBar.setOnTouchListener(new View.OnTouchListener() {
            private int startX, startY;
            private float downX, downY;
            private boolean dragging = false;

            @Override
            public boolean onTouch(View v, MotionEvent e) {
                switch (e.getAction()) {
                    case MotionEvent.ACTION_DOWN:
                        startX = lp.x;
                        startY = lp.y;
                        downX = e.getRawX();
                        downY = e.getRawY();
                        dragging = false;
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        float dx = e.getRawX() - downX;
                        float dy = e.getRawY() - downY;
                        if (!dragging && (Math.abs(dx) > UiUtil.dp(OverlayService.this, 4)
                                || Math.abs(dy) > UiUtil.dp(OverlayService.this, 4))) {
                            dragging = true;
                        }
                        if (dragging) {
                            lp.x = startX + (int) dx;
                            lp.y = startY + (int) dy;
                            wm.updateViewLayout(panelView, lp);
                        }
                        return true;
                    default:
                        return false;
                }
            }
        });

        panel.findViewById(R.id.btn_close).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                hidePanel();
            }
        });
        panel.findViewById(R.id.btn_app_icon).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                hidePanel();
            }
        });
        panel.findViewById(R.id.btn_eye).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                hidePanel();
            }
        });
        panel.findViewById(R.id.btn_pause).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                // 第一阶段：暂停 = 最小化面板（回到悬浮球）
                hidePanel();
            }
        });
        panel.findViewById(R.id.btn_refresh).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                // 刷新 = 重跑最近一次搜索（模拟值更新）
                if (!lastSearchValue.isEmpty()) {
                    results = mockRepo.search(lastSearchValue, currentType, currentMode);
                    searchAdapter.setItems(results);
                    updateResultCount();
                    Toast.makeText(OverlayService.this, R.string.searching, Toast.LENGTH_SHORT).show();
                }
            }
        });

        tvResultCount = panel.findViewById(R.id.tv_result_count);

        // 三个 tab（GG 书签）
        final TextView btnSearch = panel.findViewById(R.id.btn_tab_search);
        final TextView btnSaved = panel.findViewById(R.id.btn_tab_saved);
        final TextView btnFreeze = panel.findViewById(R.id.btn_tab_freeze);

        // 内容区 inflate 三个页面
        FrameLayout container = panel.findViewById(R.id.content_container);
        panelSearch = (LinearLayout) LayoutInflater.from(this)
                .inflate(R.layout.panel_search, container, false);
        panelSaved = (LinearLayout) LayoutInflater.from(this)
                .inflate(R.layout.panel_saved, container, false);
        panelFreeze = (LinearLayout) LayoutInflater.from(this)
                .inflate(R.layout.panel_freeze, container, false);

        container.addView(panelSearch);
        container.addView(panelSaved);
        container.addView(panelFreeze);

        initSearchPanel(panelSearch);
        initSavedPanel(panelSaved);
        initFreezePanel(panelFreeze);

        initToolbar(panel);

        switchTab(btnSearch, btnSaved, btnFreeze);

        btnSearch.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                switchTab(btnSearch, btnSaved, btnFreeze);
            }
        });
        btnSaved.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                switchTab(btnSaved, btnSearch, btnFreeze);
            }
        });
        btnFreeze.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                switchTab(btnFreeze, btnSearch, btnSaved);
            }
        });
    }

    private void switchTab(TextView active, TextView... others) {
        active.setBackgroundResource(R.drawable.tab_sel);
        active.setTextColor(getColor(R.color.on_primary));
        for (TextView b : others) {
            b.setBackgroundResource(0);
            b.setTextColor(getColor(R.color.text_secondary));
        }
        boolean showSearch = active == panelView.findViewById(R.id.btn_tab_search);
        boolean showSaved = active == panelView.findViewById(R.id.btn_tab_saved);
        boolean showFreeze = active == panelView.findViewById(R.id.btn_tab_freeze);
        if (panelSearch != null) panelSearch.setVisibility(showSearch ? View.VISIBLE : View.GONE);
        if (panelSaved != null) panelSaved.setVisibility(showSaved ? View.VISIBLE : View.GONE);
        if (panelFreeze != null) panelFreeze.setVisibility(showFreeze ? View.VISIBLE : View.GONE);
    }

    /** GG 快捷工具栏：图标按钮行（搜索 tab 的批量操作，选中结果时可用） */
    private void initToolbar(final View panel) {
        final LinearLayout bar = panel.findViewById(R.id.toolbar_search);
        bar.removeAllViews();
        addToolbarIcon(bar, R.drawable.ic_new_search, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                View et = panelSearch.findViewById(R.id.et_value);
                if (et instanceof EditText) ((EditText) et).setText("");
                results.clear();
                searchAdapter.setItems(results);
                updateResultCount();
            }
        });
        addToolbarIcon(bar, R.drawable.ic_search, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                View et = panelSearch.findViewById(R.id.et_value);
                doSearch(et instanceof EditText ? ((EditText) et).getText().toString() : "");
            }
        });
        addToolbarIcon(bar, R.drawable.ic_add_saved, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                for (SearchResult r : searchAdapter.selected()) {
                    dataStore.addToSaved(r);
                }
                Toast.makeText(OverlayService.this, R.string.menu_add_saved, Toast.LENGTH_SHORT).show();
            }
        });
        addToolbarIcon(bar, R.drawable.ic_freeze, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                for (SearchResult r : searchAdapter.selected()) {
                    dataStore.addToSaved(r);
                    SavedItem item = findSaved(r.address);
                    if (item != null) dataStore.toggleFrozen(item);
                }
                Toast.makeText(OverlayService.this, R.string.menu_freeze, Toast.LENGTH_SHORT).show();
            }
        });
        addToolbarIcon(bar, R.drawable.ic_edit, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                List<SearchResult> sel = searchAdapter.selected();
                if (sel.isEmpty()) return;
                final SearchResult first = sel.get(0);
                new EditValueDialog(OverlayService.this, first.addressHex(), first.value,
                        new EditValueDialog.OnApply() {
                            @Override
                            public void apply(String newValue) {
                                for (SearchResult r : searchAdapter.selected()) {
                                    r.value = newValue;
                                }
                                searchAdapter.notifyDataSetChanged();
                                Toast.makeText(OverlayService.this, R.string.edit_success, Toast.LENGTH_SHORT).show();
                            }
                        }).show();
            }
        });
    }

    private void addToolbarIcon(LinearLayout bar, int iconRes, View.OnClickListener l) {
        ImageButton b = new ImageButton(this);
        b.setImageResource(iconRes);
        b.setBackgroundColor(getColor(R.color.card));
        b.setPadding(UiUtil.dp(this, 12), UiUtil.dp(this, 8), UiUtil.dp(this, 12), UiUtil.dp(this, 8));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                UiUtil.dp(this, 48), UiUtil.dp(this, 48));
        lp.gravity = Gravity.CENTER_VERTICAL;
        b.setLayoutParams(lp);
        b.setOnClickListener(l);
        bar.addView(b);
    }

    // ==================== 搜索页 ====================

    private void initSearchPanel(final View panel) {
        final EditText etValue = panel.findViewById(R.id.et_value);
        final Spinner spType = panel.findViewById(R.id.sp_type);
        final Spinner spRange = panel.findViewById(R.id.sp_range);
        final Spinner spMode = panel.findViewById(R.id.sp_mode);
        final Button btnNew = panel.findViewById(R.id.btn_new_search);
        final Button btnSearch = panel.findViewById(R.id.btn_search);
        final ListView list = panel.findViewById(R.id.list_results);
        final TextView tvNoResults = panel.findViewById(R.id.tv_no_results);

        // 类型
        final List<String> types = new ArrayList<>();
        for (ValueType t : ValueType.values()) types.add(t.label);
        ArrayAdapter<String> typeAdapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, types);
        typeAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spType.setAdapter(typeAdapter);
        spType.setSelection(2); // DWORD
        spType.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                currentType = ValueType.values()[position];
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });

        // 范围（可自定义：弹出 RegionDialog）
        final List<String> ranges = new ArrayList<>(Arrays.asList(
                getString(R.string.range_all),
                getString(R.string.range_heap),
                getString(R.string.range_lib),
                getString(R.string.range_stack),
                getString(R.string.range_ca)));
        ArrayAdapter<String> rangeAdapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, ranges);
        rangeAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spRange.setAdapter(rangeAdapter);
        spRange.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                currentRange = (String) parent.getItemAtPosition(position);
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });
        // 自定义范围：长按范围 Spinner → RegionDialog
        spRange.setOnLongClickListener(new View.OnLongClickListener() {
            @Override
            public boolean onLongClick(View v) {
                openRegionDialog(spRange, ranges);
                return true;
            }
        });

        // 搜索模式
        final List<String> modes = new ArrayList<>(Arrays.asList(
                getString(R.string.mode_exact),
                getString(R.string.mode_greater),
                getString(R.string.mode_less),
                getString(R.string.mode_changed),
                getString(R.string.mode_unchanged)));
        ArrayAdapter<String> modeAdapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, modes);
        modeAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spMode.setAdapter(modeAdapter);
        spMode.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                currentMode = position;
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });

        // 搜索
        searchAdapter = new SearchAdapter(this);
        searchAdapter.setSelectionListener(new SearchAdapter.OnSelectionChanged() {
            @Override
            public void onChanged(int selectedCount) {
                updateResultCount();
            }
        });
        list.setAdapter(searchAdapter);
        list.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            @Override
            public void onItemClick(AdapterView<?> parent, View view, int position, long id) {
                // 单击由 adapter 处理（CheckBox 切换）；这里触发重绘
                searchAdapter.notifyDataSetChanged();
            }
        });
        list.setOnItemLongClickListener(new AdapterView.OnItemLongClickListener() {
            @Override
            public boolean onItemLongClick(AdapterView<?> parent, View view, final int position, long id) {
                showItemContextMenu(searchAdapter.getItem(position));
                return true;
            }
        });

        btnSearch.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                doSearch(etValue.getText().toString());
            }
        });

        // GG 符号切换（= / ≠）：点击在相等/不相等间切换
        final TextView tvSign = panel.findViewById(R.id.tv_sign);
        tvSign.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if ("=".equals(tvSign.getText().toString())) {
                    tvSign.setText("≠");
                    currentMode = 2; // Less Than 近似（Mock：当作不等于处理）
                } else {
                    tvSign.setText("=");
                    currentMode = 0;
                }
            }
        });
        // HEX 快捷按钮：在值前加 "h" 前缀标记十六进制（Mock 仅演示）
        panel.findViewById(R.id.btn_hex).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                String t = etValue.getText().toString();
                if (!t.startsWith("h")) {
                    etValue.setText("h" + t);
                } else {
                    etValue.setText(t.substring(1));
                }
            }
        });
        btnNew.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                etValue.setText("");
                results.clear();
                searchAdapter.setItems(results);
                updateResultCount();
            }
        });
    }

    private SavedItem findSaved(long address) {
        for (SavedItem s : dataStore.saved()) {
            if (s.address == address) return s;
        }
        return null;
    }

    private void doSearch(String raw) {
        if (raw == null || raw.trim().isEmpty()) {
            Toast.makeText(this, R.string.edit_invalid, Toast.LENGTH_SHORT).show();
            return;
        }
        lastSearchValue = raw.trim();
        results = mockRepo.search(raw, currentType, currentMode);
        searchAdapter.setItems(results);
        updateResultCount();
    }

    private void updateResultCount() {
        if (tvResultCount != null) {
            int total = results == null ? 0 : results.size();
            tvResultCount.setText(getString(R.string.result_count, total));
        }
        if (searchAdapter != null) {
            int n = searchAdapter.getCount();
            ListView list = null;
            if (panelSearch != null) list = panelSearch.findViewById(R.id.list_results);
            if (list != null) {
                TextView empty = panelSearch.findViewById(R.id.tv_no_results);
                empty.setVisibility(n == 0 ? View.VISIBLE : View.GONE);
            }
        }
    }

    private void openRegionDialog(final Spinner spRange, final List<String> ranges) {
        new RegionDialog(this, ranges, currentRange,
                new RegionDialog.OnRegionSelected() {
                    @Override
                    public void onSelected(String region) {
                        currentRange = region;
                        int idx = ranges.indexOf(region);
                        if (idx >= 0) {
                            spRange.setSelection(idx);
                        }
                    }
                }).show();
    }

    // ==================== 长按菜单 ====================

    private void showItemContextMenu(final SearchResult item) {
        final String[] items = {
                getString(R.string.menu_modify),
                getString(R.string.menu_add_saved),
                item.region.equals("Anonymous") ? getString(R.string.menu_freeze) : getString(R.string.menu_freeze),
                getString(R.string.menu_details),
                getString(R.string.menu_copy_address),
                getString(R.string.menu_cancel),
        };
        new android.app.AlertDialog.Builder(this, android.R.style.Theme_Black_NoTitleBar)
                .setItems(items, new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface d, int which) {
                        switch (which) {
                            case 0: // 修改数值
                                new EditValueDialog(OverlayService.this, item.addressHex(), item.value,
                                        new EditValueDialog.OnApply() {
                                            @Override
                                            public void apply(String newValue) {
                                                item.value = newValue;
                                                searchAdapter.notifyDataSetChanged();
                                                Toast.makeText(OverlayService.this, R.string.edit_success, Toast.LENGTH_SHORT).show();
                                            }
                                        }).show();
                                break;
                            case 1: // 添加到保存列表
                                dataStore.addToSaved(item);
                                Toast.makeText(OverlayService.this, R.string.menu_add_saved, Toast.LENGTH_SHORT).show();
                                break;
                            case 2: // 冻结
                                dataStore.addToSaved(item);
                                SavedItem si = findSaved(item.address);
                                if (si != null) dataStore.toggleFrozen(si);
                                Toast.makeText(OverlayService.this, R.string.menu_freeze, Toast.LENGTH_SHORT).show();
                                break;
                            case 3: // 详情
                                showDetails(item);
                                break;
                            case 4: // 复制地址
                                copyAddress(item);
                                break;
                            default:
                                break;
                        }
                    }
                })
                .show();
    }

    private void showDetails(SearchResult item) {
        String msg = "地址: " + item.addressHex() + "\n"
                + "值: " + item.value + "\n"
                + "区域: " + item.region + "\n"
                + "类型: " + item.type.label;
        new android.app.AlertDialog.Builder(this, android.R.style.Theme_Black_NoTitleBar)
                .setTitle(getString(R.string.menu_details))
                .setMessage(msg)
                .setPositiveButton(getString(R.string.yes), null)
                .show();
    }

    private void copyAddress(SearchResult item) {
        ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        cm.setPrimaryClip(ClipData.newPlainText("address", item.addressHex()));
        Toast.makeText(this, getString(R.string.menu_copy_address) + " " + item.addressHex(),
                Toast.LENGTH_SHORT).show();
    }

    // ==================== 保存页 ====================

    private void initSavedPanel(View panel) {
        final ListView list = panel.findViewById(R.id.list_saved);
        final TextView empty = panel.findViewById(R.id.tv_saved_empty);
        savedAdapter = new SavedAdapter(this, dataStore.saved(), new SavedAdapter.Actions() {
            @Override
            public void onToggleFrozen(SavedItem item) {
                dataStore.toggleFrozen(item);
            }

            @Override
            public void onDelete(final SavedItem item) {
                new android.app.AlertDialog.Builder(OverlayService.this, android.R.style.Theme_Black_NoTitleBar)
                        .setMessage(R.string.delete_confirm)
                        .setPositiveButton(R.string.yes, new android.content.DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(android.content.DialogInterface d, int w) {
                                dataStore.removeSaved(item);
                            }
                        })
                        .setNegativeButton(R.string.no, null)
                        .show();
            }

            @Override
            public void onEdit(final SavedItem item) {
                new EditValueDialog(OverlayService.this, item.addressHex(), item.value,
                        new EditValueDialog.OnApply() {
                            @Override
                            public void apply(String newValue) {
                                dataStore.updateSavedValue(item, newValue);
                                Toast.makeText(OverlayService.this, R.string.edit_success, Toast.LENGTH_SHORT).show();
                            }
                        }).show();
            }
        });
        list.setAdapter(savedAdapter);
        updateSavedEmpty(empty);
    }

    private void updateSavedEmpty(TextView empty) {
        if (empty != null) {
            empty.setVisibility(dataStore.saved().isEmpty() ? View.VISIBLE : View.GONE);
        }
    }

    // ==================== 冻结页 ====================

    private void initFreezePanel(View panel) {
        final ListView list = panel.findViewById(R.id.list_freeze);
        final TextView empty = panel.findViewById(R.id.tv_freeze_empty);
        freezeAdapter = new FreezeAdapter(this, dataStore.frozen(), new FreezeAdapter.Actions() {
            @Override
            public void onToggle(FreezeItem item) {
                dataStore.setFrozenEnabled(item, !item.enabled);
            }

            @Override
            public void onEdit(final FreezeItem item) {
                new EditValueDialog(OverlayService.this, item.addressHex(), item.value,
                        new EditValueDialog.OnApply() {
                            @Override
                            public void apply(String newValue) {
                                dataStore.updateFrozenValue(item, newValue);
                                Toast.makeText(OverlayService.this, R.string.edit_success, Toast.LENGTH_SHORT).show();
                            }
                        }).show();
            }

            @Override
            public void onDelete(final FreezeItem item) {
                new android.app.AlertDialog.Builder(OverlayService.this, android.R.style.Theme_Black_NoTitleBar)
                        .setMessage(R.string.delete_confirm)
                        .setPositiveButton(R.string.yes, new android.content.DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(android.content.DialogInterface d, int w) {
                                dataStore.removeFrozen(item);
                            }
                        })
                        .setNegativeButton(R.string.no, null)
                        .show();
            }
        });
        list.setAdapter(freezeAdapter);
        updateFreezeEmpty(empty);
    }

    private void updateFreezeEmpty(TextView empty) {
        if (empty != null) {
            empty.setVisibility(dataStore.frozen().isEmpty() ? View.VISIBLE : View.GONE);
        }
    }

    // ==================== 生命周期 ====================

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        running = false;
        dataStore.removeListener(dataListener);
        hidePanel();
        removeBall();
        super.onDestroy();
    }
}
