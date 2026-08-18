package com.ggmcp.tool.util;

import android.content.Context;
import android.content.SharedPreferences;

/** 悬浮球/面板位置持久化 */
public final class Prefs {

    private static final String NAME = "gg_tool_prefs";

    private Prefs() {
    }

    public static SharedPreferences get(Context c) {
        return c.getApplicationContext().getSharedPreferences(NAME, Context.MODE_PRIVATE);
    }

    public static int getBallX(Context c) {
        return get(c).getInt("ball_x", 0);
    }

    public static int getBallY(Context c) {
        return get(c).getInt("ball_y", 200);
    }

    public static void saveBallPos(Context c, int x, int y) {
        get(c).edit().putInt("ball_x", x).putInt("ball_y", y).apply();
    }

    public static float getBallAlpha(Context c) {
        return get(c).getFloat("ball_alpha", 1.0f);
    }

    public static void saveBallAlpha(Context c, float alpha) {
        get(c).edit().putFloat("ball_alpha", alpha).apply();
    }

    public static int getBallSize(Context c) {
        return get(c).getInt("ball_size", 44);
    }

    public static void saveBallSize(Context c, int size) {
        get(c).edit().putInt("ball_size", size).apply();
    }
}
