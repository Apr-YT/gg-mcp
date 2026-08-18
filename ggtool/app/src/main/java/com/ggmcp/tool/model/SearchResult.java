package com.ggmcp.tool.model;

/** 搜索结果条目（第一阶段 Mock 数据模型） */
public class SearchResult {
    public final long address;
    public final String region;      // 所属区域名（Anonymous / libxxx.so / Heap…）
    public final ValueType type;
    public String value;             // 当前值（字符串形式，随类型/模式显示）
    public boolean selected;         // 多选状态

    public SearchResult(long address, String region, ValueType type, String value) {
        this.address = address;
        this.region = region;
        this.type = type;
        this.value = value;
    }

    public String addressHex() {
        return String.format("0x%08X", address);
    }

    @Override
    public boolean equals(Object o) {
        return o instanceof SearchResult && ((SearchResult) o).address == address;
    }

    @Override
    public int hashCode() {
        return (int) (address ^ (address >>> 32));
    }
}
