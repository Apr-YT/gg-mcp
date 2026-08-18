package com.ggmcp.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.util.DisplayMetrics;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.TextView;

import org.json.JSONObject;

/**
 * 悬浮窗实时监控前台服务。
 * 悬浮球（54dp GG 圆球，可拖动/点击展开，连接状态边框 + 未读红点 + 断点闪红震动）
 * + 悬浮窗面板（85%×65%，4 标签：搜索/内存/保存/设置，标题栏可拖动）。
 * 持有全局共享的 {@link WireClient}：订阅 agentd 事件流 → 浮动通知 + 操作日志实时显示。
 */
public class FloatWindowService extends Service {

    /** 全局共享客户端（悬浮窗 + 各面板共用一条连接） */
    public static volatile WireClient client = new WireClient();

    private static final int NOTIF_ID = 1;
    private static final String CHANNEL_ID = "ggmcp_float";

    private WindowManager wm;
    private View ballView, ballUnread;
    private View panelView;                 // OverlayUi 构建的悬浮窗根视图
    private WindowManager.LayoutParams ballLp, panelLp;
    private OverlayUi ui;
    private final Handler h = new Handler(Looper.getMainLooper());
    private View alertView;                 // 面板未展开时的断点命中临时弹窗
    private SharedPreferences prefs;

    // 拖动状态
    private int dragMode = 0;               // 0=无 1=悬浮球 2=面板标题栏 3=右下角缩放
    private float downRawX, downRawY, downWinX, downWinY;
    private int startW, startH;
    private boolean breathing = false;

    @Override
    public void onCreate() {
        super.onCreate();
        prefs = getSharedPreferences("ggmcp", MODE_PRIVATE);
        wm = (WindowManager) getSystemService(WINDOW_SERVICE);
        createChannel();
        startForeground(NOTIF_ID, buildNotification("GG-MCP 悬浮监控运行中"));

        showBall();
        ui = new OverlayUi(this, client);
        panelView = ui.getRootView();

        // 设置页 → 服务回调
        ui.setSettingsListener(new OverlayUi.SettingsListener() {
            @Override public void onBallSizeChanged(int dp) { applyBallSize(dp); }
            @Override public void onFontScaleChanged(float scale) { /* 窗口内部已应用 */ }
        });

        client.setEventListener(ev -> h.post(() -> {
            if (ui != null) ui.appendEvent(ev);
            if ("watch_hit".equals(ev.optString("type"))) {
                boolean notifyOn = prefs.getBoolean("notify_bp", true);
                if (notifyOn) {
                    ballHitFlash();
                    // 面板未展开 → 独立弹窗 + 未读红点
                    if (panelView == null || !panelView.isAttachedToWindow()) {
                        showHitAlert(ev);
                        if (ballUnread != null) ballUnread.setVisibility(View.VISIBLE);
                    }
                }
            }
        }));
        client.setConnStateListener((ok, err) -> h.post(() -> {
            if (ui != null) ui.setConnState(ok, err);
            updateNotification(ok ? "GG-MCP 悬浮监控 · 已连接" : "GG-MCP 悬浮监控 · 未连接");
            setBallState(ok);
        }));
        client.startAutoReconnect(EngineManager.PORT);
    }

    /** 悬浮球状态：ok=蓝边正常 / bad=红边断开+呼吸动画；断点命中闪红 3 次 */
    private void setBallState(boolean connected) {
        if (ballView == null) return;
        h.post(() -> {
            try {
                ballView.setBackgroundResource(connected ? R.drawable.ball_ok : R.drawable.ball_bad);
                if (connected) {
                    stopBreathing();
                } else {
                    startBreathing();
                }
            } catch (Exception ignored) {}
        });
    }

    private void startBreathing() {
        if (ballView == null || breathing) return;
        if (!animationsEnabled()) {            // 系统「移除动画」开启时不做呼吸
            ballView.setAlpha(1f);
            return;
        }
        breathing = true;
        ballView.animate().cancel();
        ballView.animate().alpha(0.35f).setDuration(700)
                .withEndAction(() -> {
                    if (breathing && ballView != null) {
                        ballView.animate().alpha(1f).setDuration(700)
                                .withEndAction(() -> { if (breathing) startBreathing(); }).start();
                    }
                }).start();
    }

    /** 系统动画缩放为 0（开启“移除动画”）时不运行动画。 */
    private boolean animationsEnabled() {
        try {
            return android.provider.Settings.Global.getFloat(getContentResolver(),
                    android.provider.Settings.Global.ANIMATOR_DURATION_SCALE, 1f) != 0f;
        } catch (Exception e) {
            return true;
        }
    }

    private void stopBreathing() {
        breathing = false;
        if (ballView != null) {
            ballView.animate().cancel();
            ballView.setAlpha(1f);
        }
    }

    private void ballHitFlash() {
        if (ballView == null) return;
        h.post(() -> {
            for (int i = 0; i < 3; i++) {
                h.postDelayed(() -> {
                    if (ballView == null) return;
                    try { ballView.setBackgroundResource(R.drawable.ball_hit); } catch (Exception ignored) {}
                }, i * 260L);
                h.postDelayed(() -> {
                    if (ballView == null) return;
                    try { ballView.setBackgroundResource(R.drawable.ball_ok); } catch (Exception ignored) {}
                }, i * 260L + 130L);
            }
            if (prefs.getBoolean("vibrate", true)) vibrate(180);
        });
    }

    private void vibrate(long ms) {
        try {
            Vibrator v = (Vibrator) getSystemService(Context.VIBRATOR_SERVICE);
            if (v == null || !v.hasVibrator()) return;
            if (Build.VERSION.SDK_INT >= 26) {
                v.vibrate(VibrationEffect.createOneShot(ms, VibrationEffect.DEFAULT_AMPLITUDE));
            } else {
                v.vibrate(ms);
            }
        } catch (Exception ignored) {}
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    @Override
    public void onDestroy() {
        client.stopAutoReconnect();
        client.close();
        try { if (ballView != null) wm.removeView(ballView); } catch (Exception ignored) {}
        try { if (panelView != null) wm.removeView(panelView); } catch (Exception ignored) {}
        try { if (alertView != null) wm.removeView(alertView); } catch (Exception ignored) {}
        super.onDestroy();
    }

    // ---------------- 悬浮球 ----------------

    private void showBall() {
        int size = prefs.getInt("ball_size", 54);
        ballView = LayoutInflater.from(this).inflate(R.layout.float_ball, null);
        ballUnread = ballView.findViewById(R.id.ballUnread);
        ballLp = new WindowManager.LayoutParams(
                dp(size), dp(size),
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
        ballLp.gravity = Gravity.TOP | Gravity.START;
        ballLp.x = dp(16);
        ballLp.y = (wm.getDefaultDisplay().getHeight() - ballLp.height) / 2;   // 默认左侧垂直居中
        try {
            wm.addView(ballView, ballLp);
        } catch (Exception e) {
            android.util.Log.e("GGMCP", "addView ball failed: " + e.getMessage());
            ballView = null;
            return;
        }

        ballView.setOnTouchListener((v, ev) -> {
            switch (ev.getAction()) {
                case MotionEvent.ACTION_DOWN:
                    dragMode = 1;
                    downRawX = ev.getRawX();
                    downRawY = ev.getRawY();
                    downWinX = ballLp.x;
                    downWinY = ballLp.y;
                    return true;
                case MotionEvent.ACTION_MOVE:
                    if (dragMode == 1) {
                        ballLp.x = (int) (downWinX + (ev.getRawX() - downRawX));
                        ballLp.y = (int) (downWinY + (ev.getRawY() - downRawY));
                        wm.updateViewLayout(ballView, ballLp);
                    }
                    return true;
                case MotionEvent.ACTION_UP:
                    if (dragMode == 1) {
                        boolean moved = Math.abs(ev.getRawX() - downRawX) > dp(10)
                                || Math.abs(ev.getRawY() - downRawY) > dp(10);
                        if (!moved) togglePanel();   // 点击 → 展开/收起
                    }
                    dragMode = 0;
                    return true;
            }
            return false;
        });
    }

    private void applyBallSize(int dpSize) {
        if (ballLp == null) return;
        ballLp.width = dp(dpSize);
        ballLp.height = dp(dpSize);
        try {
            wm.updateViewLayout(ballView, ballLp);
        } catch (Exception ignored) {}
    }

    // ---------------- 悬浮窗面板 ----------------

    private void togglePanel() {
        try {
            if (panelView.isAttachedToWindow()) {
                wm.removeView(panelView);
            } else {
                int w = wm.getDefaultDisplay().getWidth();
                int hh = wm.getDefaultDisplay().getHeight();
                // 横屏屏高低：窗口默认高度提到 80%，避免内容区过矮；竖屏 65%
                boolean landscape = getResources().getConfiguration().orientation
                        == android.content.res.Configuration.ORIENTATION_LANDSCAPE;
                panelLp = new WindowManager.LayoutParams(
                        (int) (w * 0.85f),
                        (int) (hh * (landscape ? 0.80f : 0.65f)),
                        WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                        WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                        PixelFormat.TRANSLUCENT);
                panelLp.gravity = Gravity.TOP | Gravity.START;
                panelLp.x = (w - panelLp.width) / 2;
                panelLp.y = (int) (hh * 0.10f);
                wm.addView(panelView, panelLp);

                // 仅标题栏可拖动
                View title = panelView.findViewById(R.id.titleBar);
                title.setOnTouchListener((v, ev) -> {
                    switch (ev.getAction()) {
                        case MotionEvent.ACTION_DOWN:
                            dragMode = 2;
                            downRawX = ev.getRawX();
                            downRawY = ev.getRawY();
                            downWinX = panelLp.x;
                            downWinY = panelLp.y;
                            return true;
                        case MotionEvent.ACTION_MOVE:
                            if (dragMode == 2) {
                                panelLp.x = (int) (downWinX + (ev.getRawX() - downRawX));
                                panelLp.y = (int) (downWinY + (ev.getRawY() - downRawY));
                                wm.updateViewLayout(panelView, panelLp);
                            }
                            return true;
                        case MotionEvent.ACTION_UP:
                            dragMode = 0;
                            return true;
                    }
                    return false;
                });
                // 右下角 ◢ 缩放
                View resize = panelView.findViewById(R.id.btnResize);
                resize.setOnTouchListener((v, ev) -> {
                    switch (ev.getAction()) {
                        case MotionEvent.ACTION_DOWN:
                            dragMode = 3;
                            downRawX = ev.getRawX();
                            downRawY = ev.getRawY();
                            startW = panelLp.width;
                            startH = panelLp.height;
                            return true;
                        case MotionEvent.ACTION_MOVE:
                            if (dragMode == 3) {
                                int nw = startW + (int) (ev.getRawX() - downRawX);
                                int nh = startH + (int) (ev.getRawY() - downRawY);
                                int minW = dp(300), minH = dp(240);
                                int maxW = wm.getDefaultDisplay().getWidth();
                                int maxH = wm.getDefaultDisplay().getHeight();
                                if (nw < minW) nw = minW;
                                if (nh < minH) nh = minH;
                                if (nw > maxW) nw = maxW;
                                if (nh > maxH) nh = maxH;
                                panelLp.width = nw;
                                panelLp.height = nh;
                                wm.updateViewLayout(panelView, panelLp);
                            }
                            return true;
                        case MotionEvent.ACTION_UP:
                            dragMode = 0;
                            return true;
                    }
                    return false;
                });
                panelView.findViewById(R.id.btnHideWindow).setOnClickListener(v -> togglePanel());
                panelView.findViewById(R.id.btnCloseWindow).setOnClickListener(v -> stopSelf());
                if (ballUnread != null) ballUnread.setVisibility(View.GONE);   // 打开即已读
            }
        } catch (Exception ignored) {}
    }

    // ---------------- 断点命中临时弹窗（面板未展开时） ----------------

    private void showHitAlert(JSONObject ev) {
        try {
            JSONObject d = ev.optJSONObject("data");
            if (d == null) return;
            String module = d.optString("module");
            String off = d.optString("module_offset");
            String pc = d.optString("pc");
            final String text = "⚡ 断点命中! " + (module.isEmpty() ? d.optString("addr") : module + "+" + off) + " @ " + pc;

            if (alertView != null) {
                try { wm.removeView(alertView); } catch (Exception ignored) {}
            }
            TextView tv = new TextView(this);
            tv.setText(text);
            tv.setTextColor(0xFF101018);       // 红底深字，保证对比度 ≥4.5:1
            tv.setTextSize(13);
            tv.setBackgroundResource(R.drawable.alert_bg);
            tv.setPadding(dp(14), dp(10), dp(14), dp(10));
            tv.setOnClickListener(v -> {
                try { wm.removeView(alertView); } catch (Exception ignored) {}
                alertView = null;
                togglePanel();
            });
            WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
                    PixelFormat.TRANSLUCENT);
            lp.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL;
            lp.y = dp(120);
            alertView = tv;
            wm.addView(tv, lp);
            h.postDelayed(() -> {
                if (alertView != null) {
                    try { wm.removeView(alertView); } catch (Exception ignored) {}
                    alertView = null;
                }
            }, 3000);
        } catch (Exception ignored) {}
    }

    // ---------------- 通知 ----------------

    private void createChannel() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        NotificationChannel ch = new NotificationChannel(
                CHANNEL_ID, "GG-MCP 悬浮监控", NotificationManager.IMPORTANCE_LOW);
        ch.setDescription("悬浮窗服务常驻通知");
        nm.createNotificationChannel(ch);
    }

    private Notification buildNotification(String text) {
        Notification.Builder b = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("GG-MCP")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.ic_menu_compass)
                .setOngoing(true);
        return b.build();
    }

    private void updateNotification(String text) {
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.notify(NOTIF_ID, buildNotification(text));
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
