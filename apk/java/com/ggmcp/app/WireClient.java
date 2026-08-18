package com.ggmcp.app;

import org.json.JSONObject;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.ConcurrentHashMap;

/**
 * gg-agentd wire 协议客户端（与 shared/wire_protocol.h 一致）。
 *
 * 单连接 + 后台读线程（手机悬浮窗实时监控）：
 *  - GGWF_EVENT 帧 → {@link EventListener#onEvent}（实时日志/断点命中/标注）
 *  - 普通响应帧   → 按 seq 匹配 {@link #call} 的等待者（手动操作照常同步返回）
 *  - 连接断开自动重连（指数退避 1s→2s→4s→…→30s 封顶），重连成功后自动重新订阅事件流
 *
 * 帧: 20 字节小端头 {magic,ver,cmd,seq,flags,len} + JSON 负载。
 */
public class WireClient {

    public static final int MAGIC = 0x47474D43;
    public static final int VER = 1;
    public static final int FLAG_REQ = 1;
    public static final int FLAG_ERR = 4;
    public static final int FLAG_EVENT = 8;

    // 命令码（镜像 wire_protocol.h）
    public static final int CMD_PING = 1, CMD_SELF_TEST = 2, CMD_LIST_PROCESSES = 3,
            CMD_ATTACH = 4, CMD_DETACH = 5, CMD_TARGET_INFO = 6, CMD_MODULE_LIST = 7,
            CMD_SEARCH = 8, CMD_SEARCH_UNKNOWN_INIT = 9, CMD_SEARCH_REFINE = 10,
            CMD_SEARCH_RANGE = 11, CMD_RESULT_COUNT = 12, CMD_GET_RESULTS = 13,
            CMD_CLEAR_RESULTS = 14, CMD_READ = 15, CMD_READ_BYTES = 16, CMD_HEX_DUMP = 17,
            CMD_WRITE = 18, CMD_SET_RESULTS = 19, CMD_FREEZE = 20, CMD_UNFREEZE = 21,
            CMD_MODULE_BASE = 22, CMD_CALC_OFFSET = 23, CMD_POINTER_SCAN = 24,
            CMD_PAUSE = 25, CMD_RESUME = 26, CMD_KERNEL_STATUS = 27, CMD_LOAD_GG = 28,
            CMD_CORE_VERSIONS = 29, CMD_SHELL = 30, CMD_DEVICE_INFO = 31, CMD_WATCH = 32,
            CMD_DISASSEMBLE = 33, CMD_VIEW_MEMORY = 34, CMD_RESOLVE_SYMBOL = 35,
            CMD_POINTER_CHAIN = 36, CMD_SUBSCRIBE_EVENTS = 37, CMD_UNSUBSCRIBE_EVENTS = 38,
            CMD_RELAY_EVENT = 39;

    // 值类型 / 区域 / 细化操作
    public static final int VT_BYTE = 1, VT_WORD = 2, VT_DWORD = 3, VT_QWORD = 4, VT_FLOAT = 5, VT_DOUBLE = 6;
    public static final int REGION_ANONYMOUS = 1, REGION_WRITABLE = 2, REGION_ALL = 3;
    public static final int REFINE_INCREASED = 1, REFINE_DECREASED = 2, REFINE_UNCHANGED = 3, REFINE_CHANGED = 4;

    /** 事件监听（读线程回调，需自行切主线程更新 UI） */
    public interface EventListener { void onEvent(JSONObject ev); }

    /** 连接状态监听 */
    public interface ConnStateListener { void onStateChanged(boolean connected, String error); }

    private Socket sock;
    private DataInputStream in;
    private DataOutputStream out;
    private volatile int seq = 0;
    private volatile boolean running = false;
    private Thread reader;
    private volatile EventListener eventListener;
    private volatile ConnStateListener connListener;
    private volatile int autoPort = 0;      // 0 = 未开启自动重连
    private volatile boolean autoStop = false;
    private Thread autoThread;
    public String lastError = "";

    private final ConcurrentHashMap<Integer, Pending> pending = new ConcurrentHashMap<>();

    private static class Pending {
        JSONObject result;
        int errCode = 0;
        String errMsg;
        boolean done;
    }

    public boolean isConnected() {
        return running && sock != null && sock.isConnected() && !sock.isClosed();
    }

    public void setEventListener(EventListener l) { eventListener = l; }
    public void setConnStateListener(ConnStateListener l) { connListener = l; }

    /** 建立连接并启动读线程；成功后自动订阅事件流。 */
    public synchronized boolean connect(int port) {
        closeInternal();
        try {
            sock = new Socket();
            sock.connect(new InetSocketAddress("127.0.0.1", port), 5000);
            sock.setSoTimeout(0);           // 阻塞读，由读线程管理
            in = new DataInputStream(sock.getInputStream());
            out = new DataOutputStream(sock.getOutputStream());
            running = true;
            reader = new Thread(this::readLoop, "wire-reader");
            reader.setDaemon(true);
            reader.start();
            lastError = "";
            subscribeEvents();              // 事件订阅（幂等，失败由下次重连补）
            return true;
        } catch (Exception e) {
            lastError = e.getMessage();
            sock = null;
            return false;
        }
    }

    public synchronized void close() {
        autoStop = true;
        closeInternal();
    }

    private void closeInternal() {
        running = false;
        try { if (sock != null) sock.close(); } catch (IOException ignored) {}
        sock = null;
        in = null;
        out = null;
        // 唤醒所有等待中的 call()（标记连接中断，避免返回 null）
        for (Pending p : pending.values()) {
            synchronized (p) {
                if (!p.done) {
                    p.errCode = -2;
                    p.errMsg = "connection closed";
                    p.done = true;
                    p.notifyAll();
                }
            }
        }
        pending.clear();
    }

    /** 自动重连：断开后按 1s→2s→4s→…→30s 封顶退避重连，成功后自动订阅。 */
    public synchronized void startAutoReconnect(final int port) {
        if (autoThread != null && autoThread.isAlive()) return;
        autoPort = port;
        autoStop = false;
        autoThread = new Thread(() -> {
            long delay = 1000;
            while (!autoStop) {
                if (!isConnected()) {
                    if (connect(port)) {
                        delay = 1000;
                        ConnStateListener l = connListener;
                        if (l != null) l.onStateChanged(true, null);
                    } else {
                        ConnStateListener l = connListener;
                        if (l != null) l.onStateChanged(false, lastError);
                    }
                }
                try { Thread.sleep(delay); } catch (InterruptedException e) { break; }
                delay = Math.min(delay * 2, 30000);
            }
        }, "wire-reconnect");
        autoThread.setDaemon(true);
        autoThread.start();
    }

    public void stopAutoReconnect() { autoStop = true; }

    private void subscribeEvents() {
        try {
            sendFrame(CMD_SUBSCRIBE_EVENTS, new JSONObject(), 0, false);
        } catch (Exception ignored) {}
    }

    /** 读线程：事件 → 监听器；响应 → 匹配 call() 等待者。 */
    private void readLoop() {
        String err = null;
        try {
            for (;;) {
                byte[] rb = new byte[20];
                in.readFully(rb);
                ByteBuffer rh = ByteBuffer.wrap(rb).order(ByteOrder.LITTLE_ENDIAN);
                int magic = rh.getInt();
                short ver = rh.getShort();
                rh.getShort(); // cmd
                int rseq = rh.getInt();
                int flags = rh.getInt();
                int len = rh.getInt();
                if (magic != MAGIC || ver != VER) throw new IOException("bad frame magic/ver");
                if (len < 0 || len > (1 << 20)) throw new IOException("bad len " + len);
                byte[] body = new byte[len];
                in.readFully(body);
                String raw = new String(body, "UTF-8");
                JSONObject j = new JSONObject(raw);
                if ((flags & FLAG_EVENT) != 0) {
                    JSONObject ev = j.optJSONObject("ev");
                    EventListener l = eventListener;
                    if (l != null && ev != null) l.onEvent(ev);
                    continue;
                }
                Pending p = pending.remove(rseq);
                if (p == null) continue;      // 响应迟到/无等待者 → 丢弃
                synchronized (p) {
                    if ((flags & FLAG_ERR) != 0) {
                        JSONObject e = j.optJSONObject("e");
                        p.errCode = e != null ? e.optInt("code", -1) : -1;
                        p.errMsg = e != null ? e.optString("message", "error") : "error";
                    } else {
                        p.result = j.optJSONObject("r");
                        if (p.result == null) p.result = j;   // 裸对象响应
                    }
                    p.done = true;
                    p.notifyAll();
                }
            }
        } catch (Exception e) {
            err = e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
        } finally {
            running = false;
            ConnStateListener l = connListener;
            if (l != null) l.onStateChanged(false, err != null ? err : "connection closed");
            closeInternal();
        }
    }

    private void sendFrame(int cmd, JSONObject params, int s, boolean await) throws Exception {
        JSONObject req = new JSONObject();
        req.put("p", params == null ? new JSONObject() : params);
        byte[] pl = req.toString().getBytes("UTF-8");
        ByteBuffer hdr = ByteBuffer.allocate(20).order(ByteOrder.LITTLE_ENDIAN);
        hdr.putInt(MAGIC);
        hdr.putShort((short) VER);
        hdr.putShort((short) cmd);
        hdr.putInt(s);
        hdr.putInt(FLAG_REQ);
        hdr.putInt(pl.length);
        out.write(hdr.array());
        out.write(pl);
        out.flush();
    }

    /** 调用并返回 {"r":...}；错误抛 WireException(code, message)。线程安全。 */
    public JSONObject call(int cmd, JSONObject params) throws Exception {
        if (!isConnected()) throw new WireException(-2, "engine not connected");
        int s;
        Pending p = new Pending();
        synchronized (this) {
            s = ++seq;                      // seq 分配与写帧同锁，防并发重号
            pending.put(s, p);
            sendFrame(cmd, params, s, true);
        }
        synchronized (p) {
            if (!p.done) p.wait(120000);    // 120s 超时（大范围搜索可能较久）
        }
        pending.remove(s);                  // 正常/超时/断开均在此清理（幂等）
        if (!p.done) throw new WireException(-4, "timeout waiting response");
        if (p.errCode != 0) throw new WireException(p.errCode, p.errMsg);
        return p.result;
    }

    public static class WireException extends Exception {
        public final int code;
        public WireException(int code, String msg) {
            super(msg);
            this.code = code;
        }
    }
}
