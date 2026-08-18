package com.ggmcp.tool.model;

/** 保存列表条目（可冻结） */
public class SavedItem {
    public final long address;
    public final String region;
    public final ValueType type;
    public String value;
    public boolean frozen;
    public boolean selected;

    public SavedItem(long address, String region, ValueType type, String value) {
        this.address = address;
        this.region = region;
        this.type = type;
        this.value = value;
    }

    public String addressHex() {
        return String.format("0x%08X", address);
    }

    public String meta() {
        return region + " · " + type.label;
    }

    @Override
    public boolean equals(Object o) {
        return o instanceof SavedItem && ((SavedItem) o).address == address;
    }

    @Override
    public int hashCode() {
        return (int) (address ^ (address >>> 32));
    }
}
