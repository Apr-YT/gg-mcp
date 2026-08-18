package com.ggmcp.tool.util;

import android.content.Context;

/** UI 工具：dp/sp 转换等 */
public final class UiUtil {

    private UiUtil() {
    }

    public static int dp(Context c, float dp) {
        return Math.round(dp * c.getResources().getDisplayMetrics().density);
    }

    public static int sp(Context c, float sp) {
        return Math.round(sp * c.getResources().getDisplayMetrics().scaledDensity);
    }
}
