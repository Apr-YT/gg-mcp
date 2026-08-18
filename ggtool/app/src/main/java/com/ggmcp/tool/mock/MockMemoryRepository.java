package com.ggmcp.tool.mock;

import com.ggmcp.tool.model.SearchResult;
import com.ggmcp.tool.model.ValueType;

import java.util.ArrayList;
import java.util.List;
import java.util.Random;

/**
 * Mock 内存仓库：第一阶段模拟搜索/读取/写入。
 * 设计为后续可替换为真实 Memory Engine（同接口）。
 */
public class MockMemoryRepository {

    private static final String[] REGIONS = {"Anonymous", "Anonymous", "Anonymous", "libxxx.so", "Heap", "Stack"};
    private static final long BASE = 0x7A100000L;

    private final Random random = new Random(42L);

    /** 模拟一次搜索：生成 0..max 条结果，值接近搜索值或按模式分布 */
    public List<SearchResult> search(String rawValue, ValueType type, int mode) {
        List<SearchResult> out = new ArrayList<>();
        int count = 8 + random.nextInt(20); // 8..27 条
        double v = 0;
        try {
            v = Double.parseDouble(rawValue.trim());
        } catch (NumberFormatException ignored) {
            // 非数值输入 → 生成随机值
        }
        for (int i = 0; i < count; i++) {
            long addr = BASE + (random.nextInt(0x8000) * 4L);
            String region = REGIONS[random.nextInt(REGIONS.length)];
            String val = mockValue(type, v, mode);
            out.add(new SearchResult(addr, region, type, val));
        }
        return out;
    }

    private String mockValue(ValueType type, double v, int mode) {
        switch (mode) {
            case 1: // Greater Than
                return fmt(type, v + 1 + random.nextInt(500));
            case 2: // Less Than
                return fmt(type, Math.max(0, v - 1 - random.nextInt(200)));
            case 3: // Changed
            case 4: // Unchanged
                return fmt(type, v);
            default: // Exact
                return fmt(type, v);
        }
    }

    private String fmt(ValueType type, double v) {
        switch (type) {
            case BYTE:  return String.valueOf((byte) ((int) v & 0xFF));
            case WORD:  return String.valueOf((short) ((int) v & 0xFFFF));
            case QWORD: return String.valueOf((long) v);
            case FLOAT: return String.valueOf((float) v);
            case DOUBLE:return String.valueOf(v);
            default:    return String.valueOf((int) v); // DWORD
        }
    }

    /** 模拟读取地址当前值（修改后从 Mock 内存取回） */
    public String read(long address, ValueType type) {
        return fmt(type, 100 + (address % 900));
    }

    /** 模拟写入（实际第一阶段只改 Mock 状态） */
    public void write(long address, ValueType type, String value) {
        // no-op: Mock
    }
}
