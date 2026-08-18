package com.ggmcp.tool.model;

/** 冻结列表条目（来自保存列表的冻结项） */
public class FreezeItem {
    public final long address;
    public final ValueType type;
    public String value;
    public boolean enabled;          // 是否处于冻结启用状态

    public FreezeItem(long address, ValueType type, String value) {
        this.address = address;
        this.type = type;
        this.value = value;
        this.enabled = true;
    }

    public String addressHex() {
        return String.format("0x%08X", address);
    }
}
