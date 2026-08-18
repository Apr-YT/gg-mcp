package com.ggmcp.tool.ui.editor;

import android.app.Dialog;
import android.content.Context;
import android.view.View;
import android.view.Window;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import com.ggmcp.tool.R;
import com.ggmcp.tool.ui.keyboard.NumericKeyboard;

/**
 * 修改数值 Dialog（GG 风格）：地址/当前值/新值 + 内建数字键盘。
 * 服务上下文无 Activity token → 用系统 AlertDialog 主题的 Dialog 并设置
 * TYPE_APPLICATION_OVERLAY 窗口类型（OverlayUi 约定）。
 */
public class EditValueDialog {

    public interface OnApply {
        void apply(String newValue);
    }

    private final Dialog dialog;
    private final EditText etNewValue;
    private final NumericKeyboard keyboard;
    private OnApply listener;

    public EditValueDialog(Context context, String address, String currentValue, OnApply onApply) {
        this.listener = onApply;
        this.dialog = new Dialog(context, android.R.style.Theme_Black_NoTitleBar);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        dialog.setContentView(R.layout.dialog_edit);

        Window w = dialog.getWindow();
        if (w != null) {
            w.setType(android.view.WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY);
            w.setBackgroundDrawableResource(R.drawable.panel_bg);
        }

        ((TextView) dialog.findViewById(R.id.tv_address)).setText(address);
        ((TextView) dialog.findViewById(R.id.tv_current)).setText(currentValue);
        etNewValue = dialog.findViewById(R.id.et_new_value);

        keyboard = new NumericKeyboard(context, etNewValue);
        keyboard.attachTo((LinearLayout) dialog.findViewById(R.id.keyboard_container));
        keyboard.setOnConfirmListener(new NumericKeyboard.OnConfirmListener() {
            @Override
            public void onConfirm(String text) {
                if (text == null || text.trim().isEmpty()) {
                    Toast.makeText(context, R.string.edit_invalid, Toast.LENGTH_SHORT).show();
                    return;
                }
                if (EditValueDialog.this.listener != null) {
                    EditValueDialog.this.listener.apply(text.trim());
                }
                dialog.dismiss();
            }
        });

        ((Button) dialog.findViewById(R.id.btn_ok)).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                keyboard.getRoot().findViewById(R.id.key_ok).performClick();
            }
        });
        ((Button) dialog.findViewById(R.id.btn_cancel)).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                dialog.dismiss();
            }
        });
    }

    public void show() {
        dialog.show();
    }
}
