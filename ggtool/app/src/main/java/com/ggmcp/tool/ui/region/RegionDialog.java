package com.ggmcp.tool.ui.region;

import android.app.Dialog;
import android.content.Context;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.ListView;
import android.widget.TextView;

import com.ggmcp.tool.R;

import java.util.List;

/** 区域选择对话框（单选，返回选中区域字符串） */
public class RegionDialog {

    public interface OnRegionSelected {
        void onSelected(String region);
    }

    private final Dialog dialog;
    private final OnRegionSelected listener;

    public RegionDialog(Context context, List<String> regions, String current, OnRegionSelected l) {
        this.listener = l;
        this.dialog = new Dialog(context, android.R.style.Theme_Black_NoTitleBar);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        dialog.setContentView(R.layout.dialog_region);

        Window w = dialog.getWindow();
        if (w != null) {
            w.setType(WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY);
            w.setBackgroundDrawableResource(R.drawable.panel_bg);
            w.setLayout(
                    (int) (context.getResources().getDisplayMetrics().widthPixels * 0.8f),
                    WindowManager.LayoutParams.WRAP_CONTENT);
        }

        ListView list = dialog.findViewById(R.id.list_regions);
        list.setAdapter(new ArrayAdapter<>(context,
                android.R.layout.simple_list_item_single_choice, regions));
        list.setChoiceMode(ListView.CHOICE_MODE_SINGLE);
        int idx = regions.indexOf(current);
        if (idx >= 0) list.setItemChecked(idx, true);
        list.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            @Override
            public void onItemClick(AdapterView<?> parent, View view, int position, long id) {
                String region = (String) parent.getItemAtPosition(position);
                if (RegionDialog.this.listener != null) {
                    RegionDialog.this.listener.onSelected(region);
                }
                dialog.dismiss();
            }
        });
    }

    public void show() {
        dialog.show();
    }
}
