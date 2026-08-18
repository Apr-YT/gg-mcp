package com.ggmcp.tool;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import com.ggmcp.tool.service.OverlayService;

/** 主 Activity：悬浮窗权限检查 + 启动/停止悬浮窗服务 */
public class MainActivity extends Activity {

    private TextView tvStatus;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        tvStatus = findViewById(R.id.tv_status);
        Button btnStart = findViewById(R.id.btn_start_overlay);
        Button btnStop = findViewById(R.id.btn_stop_overlay);

        btnStart.setOnClickListener(v -> {
            if (!canDrawOverlays()) {
                requestOverlayPermission();
            } else {
                startOverlayService();
            }
        });

        btnStop.setOnClickListener(v -> {
            stopService(new Intent(this, OverlayService.class));
            updateStatus();
        });

        updateStatus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        updateStatus();
        // 已有悬浮窗权限且服务未运行时自动启动（方便快速验证）
        if (canDrawOverlays() && !OverlayService.isRunning()) {
            startOverlayService();
        }
    }

    private boolean canDrawOverlays() {
        return Build.VERSION.SDK_INT < 23 || Settings.canDrawOverlays(this);
    }

    private void requestOverlayPermission() {
        if (Build.VERSION.SDK_INT >= 23) {
            Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivity(intent);
            Toast.makeText(this, R.string.overlay_permission_required, Toast.LENGTH_LONG).show();
        }
    }

    private void startOverlayService() {
        Intent i = new Intent(this, OverlayService.class);
        if (Build.VERSION.SDK_INT >= 26) {
            startForegroundService(i);
        } else {
            startService(i);
        }
        updateStatus();
    }

    private void updateStatus() {
        boolean running = isServiceRunning();
        tvStatus.setText(running ? R.string.overlay_running : R.string.start_overlay);
        tvStatus.setTextColor(getColor(running ? R.color.green : R.color.text_secondary));
    }

    private boolean isServiceRunning() {
        return OverlayService.isRunning();
    }

    /** 供 OverlayService 查询运行状态 */
    public static boolean isOverlayRunning(Context ctx) {
        return OverlayService.isRunning();
    }
}
