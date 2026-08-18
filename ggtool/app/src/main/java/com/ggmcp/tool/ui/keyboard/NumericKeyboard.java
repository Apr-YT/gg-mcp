package com.ggmcp.tool.ui.keyboard;

import android.content.Context;
import android.text.Editable;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;

import com.ggmcp.tool.R;

/**
 * 内建数字键盘（复刻 APK InternalKeyboard 的实用形态）：
 * 4×4 布局，支持数字/小数/负数/删除/清空/确认。
 * 绑定到目标 EditText（阻止系统输入法弹出）。
 */
public class NumericKeyboard {

    public interface OnConfirmListener {
        void onConfirm(String text);
    }

    private final View root;
    private final EditText target;
    private OnConfirmListener confirmListener;

    public NumericKeyboard(Context context, EditText target) {
        this.target = target;
        this.root = LayoutInflater.from(context).inflate(R.layout.view_keyboard, null);
        bindKeys();
        // 阻止系统 IME：目标框不可聚焦由调用方设置，这里兜底
        target.setShowSoftInputOnFocus(false);
    }

    /** 挂到父容器 */
    public void attachTo(LinearLayout container) {
        container.removeAllViews();
        container.addView(root);
    }

    public void setOnConfirmListener(OnConfirmListener l) {
        this.confirmListener = l;
    }

    public View getRoot() {
        return root;
    }

    private void bindKeys() {
        bindDigit(R.id.key_1, "1");
        bindDigit(R.id.key_2, "2");
        bindDigit(R.id.key_3, "3");
        bindDigit(R.id.key_4, "4");
        bindDigit(R.id.key_5, "5");
        bindDigit(R.id.key_6, "6");
        bindDigit(R.id.key_7, "7");
        bindDigit(R.id.key_8, "8");
        bindDigit(R.id.key_9, "9");
        bindDigit(R.id.key_0, "0");
        bindDigit(R.id.key_00, "00");
        bindDigit(R.id.key_dot, ".");
        bindDigit(R.id.key_minus, "-");

        root.findViewById(R.id.key_del).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Editable e = target.getText();
                if (e == null || e.length() == 0) return;
                int start = Math.max(0, target.getSelectionStart());
                int end = Math.max(0, target.getSelectionEnd());
                if (start == end) {
                    e.delete(Math.max(0, start - 1), start);
                } else {
                    e.delete(Math.min(start, end), Math.max(start, end));
                }
            }
        });

        root.findViewById(R.id.key_clear).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                target.setText("");
            }
        });

        root.findViewById(R.id.key_ok).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (confirmListener != null) {
                    confirmListener.onConfirm(target.getText() == null ? "" : target.getText().toString());
                }
            }
        });
    }

    private void bindDigit(int id, final String digit) {
        root.findViewById(id).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                int start = Math.max(0, target.getSelectionStart());
                int end = Math.max(0, target.getSelectionEnd());
                target.getText().replace(Math.min(start, end), Math.max(start, end), digit);
            }
        });
    }
}
