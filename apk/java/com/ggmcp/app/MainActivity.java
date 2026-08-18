package com.ggmcp.app;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.provider.Settings;
import android.widget.TextView;

/**
 * 悬浮窗监控入口：申请「悬浮窗」权限 → 启动 FloatWindowService → 结束。
 * 实际 UI 全部在悬浮窗里（搜索/内存/断点/标注/日志/设置）。
 */
public class MainActivity extends Activity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        findViewById(R.id.btnGrant).setOnClickListener(v -> openOverlaySettings());
        findViewById(R.id.btnStart).setOnClickListener(v -> startMonitor());
    }

    @Override
    protected void onResume() {
        super.onResume();
        TextView hint = findViewById(R.id.hintText);
        if (Settings.canDrawOverlays(this)) {
            startMonitor();
        } else {
            hint.setText("需要「悬浮窗」权限才能显示 GG-MCP 悬浮窗。\n"
                    + "授予权限后自动启动，或点下方按钮手动启动。");
        }
    }

    private void openOverlaySettings() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION));
        }
    }

    private void startMonitor() {
        if (!Settings.canDrawOverlays(this)) {
            openOverlaySettings();
            return;
        }
        Intent i = new Intent(this, FloatWindowService.class);
        try {
            startForegroundService(i);
        } catch (Exception e) {
            startService(i);
        }
        TextView hint = findViewById(R.id.hintText);
        hint.setText("悬浮监控已启动：点击悬浮球「GG」展开/收起悬浮窗。\n"
                + "悬浮窗 4 页：搜索 / 内存 / 保存 / 设置；AI 操作实时显示为浮动通知，\n"
                + "完整记录在「设置 → 操作日志」。");
    }
}
