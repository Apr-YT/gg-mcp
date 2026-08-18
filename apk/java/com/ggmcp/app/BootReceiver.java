package com.ggmcp.app;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;

/**
 * 开机自启：设置页「开机自启」开关打开后，开机自动拉起悬浮监控服务。
 */
public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context ctx, Intent intent) {
        if (intent == null || !Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) return;
        SharedPreferences prefs = ctx.getSharedPreferences("ggmcp", Context.MODE_PRIVATE);
        if (!prefs.getBoolean("boot", false)) return;
        try {
            Intent i = new Intent(ctx, FloatWindowService.class);
            if (android.os.Build.VERSION.SDK_INT >= 26) {
                ctx.startForegroundService(i);
            } else {
                ctx.startService(i);
            }
        } catch (Exception ignored) {}
    }
}
