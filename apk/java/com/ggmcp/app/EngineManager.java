package com.ggmcp.app;

import android.content.Context;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;

/**
 * 引擎管理：root 环境检测、gg-agentd 提取与启动（su + start_agentd.sh）。
 */
public class EngineManager {

    public static final int PORT = 19100;
    public static final String DIR = "/data/local/tmp/ggmcp";

    /** 执行 su 命令并返回 stdout。 */
    public static String su(String cmd) {
        try {
            Process p = Runtime.getRuntime().exec(new String[]{"su", "-c", cmd});
            StringBuilder sb = new StringBuilder();
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line;
            while ((line = r.readLine()) != null) sb.append(line).append('\n');
            p.waitFor();
            return sb.toString();
        } catch (Exception e) {
            return "";
        }
    }

    public static boolean hasRoot() {
        return su("id").contains("uid=0");
    }

    /** 把 assets 里的文件提取到应用私有目录。 */
    private static void extractAsset(Context ctx, String name, File dst) throws Exception {
        InputStream is = ctx.getAssets().open(name);
        FileOutputStream os = new FileOutputStream(dst);
        byte[] buf = new byte[65536];
        int n;
        while ((n = is.read(buf)) > 0) os.write(buf, 0, n);
        os.close();
        is.close();
        dst.setExecutable(true, false);
        dst.setReadable(true, false);
    }

    /** 确保引擎就绪：提取二进制 + su 启动 + 等待端口。 */
    public static String ensureEngine(Context ctx) {
        if (portOpen()) return "engine already running";
        try {
            File dir = ctx.getDir("engine", Context.MODE_PRIVATE); // /data/user/0/.../app_engine
            File bin = new File(dir, "gg-agentd");
            File sh = new File(dir, "start_agentd.sh");
            extractAsset(ctx, "gg-agentd", bin);
            extractAsset(ctx, "start_agentd.sh", sh);

            // 复制到 /data/local/tmp/ggmcp（root 引擎运行目录）
            su("mkdir -p " + DIR + "; chmod 777 " + DIR);
            su("cp '" + bin.getAbsolutePath() + "' " + DIR + "/gg-agentd; chmod 755 " + DIR + "/gg-agentd");
            su("cp '" + sh.getAbsolutePath() + "' " + DIR + "/start_agentd.sh; chmod 755 " + DIR + "/start_agentd.sh");

            String out = su("sh " + DIR + "/start_agentd.sh start");
            // 等待端口
            for (int i = 0; i < 20; i++) {
                if (portOpen()) return out.trim();
                try { Thread.sleep(500); } catch (InterruptedException ignored) {}
            }
            return "engine start timeout: " + out.trim();
        } catch (Exception e) {
            return "engine error: " + e.getMessage();
        }
    }

    public static boolean portOpen() {
        try {
            java.net.Socket s = new java.net.Socket("127.0.0.1", PORT);
            s.close();
            return true;
        } catch (Exception e) {
            return false;
        }
    }
}
