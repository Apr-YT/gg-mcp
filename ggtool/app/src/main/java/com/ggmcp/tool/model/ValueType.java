package com.ggmcp.tool.model;

/** 数据类型枚举（对应 GG 的 BYTE/WORD/DWORD/QWORD/FLOAT/DOUBLE/XOR） */
public enum ValueType {
    BYTE(1, "BYTE"),
    WORD(2, "WORD"),
    DWORD(4, "DWORD"),
    QWORD(8, "QWORD"),
    FLOAT(4, "FLOAT"),
    DOUBLE(8, "DOUBLE");

    public final int size;
    public final String label;

    ValueType(int size, String label) {
        this.size = size;
        this.label = label;
    }

    /** 从字符串 label 解析（不区分大小写），未知返回 DWORD */
    public static ValueType fromLabel(String s) {
        if (s == null) return DWORD;
        for (ValueType t : values()) {
            if (t.label.equalsIgnoreCase(s.trim())) return t;
        }
        return DWORD;
    }
}
