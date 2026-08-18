package com.ggmcp.tool.ui.saved;

import android.content.Context;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.ImageButton;
import android.widget.TextView;

import com.ggmcp.tool.R;
import com.ggmcp.tool.model.SavedItem;

import java.util.List;

/** 保存列表适配器：选择/冻结/删除 */
public class SavedAdapter extends BaseAdapter {

    public interface Actions {
        void onToggleFrozen(SavedItem item);
        void onDelete(SavedItem item);
        void onEdit(SavedItem item);
    }

    private final LayoutInflater inflater;
    private final List<SavedItem> items;
    private final Actions actions;
    private final String freezeLabel;
    private final String unfreezeLabel;

    public SavedAdapter(Context context, List<SavedItem> items, Actions actions) {
        this.inflater = LayoutInflater.from(context);
        this.items = items;
        this.actions = actions;
        this.freezeLabel = context.getString(R.string.menu_freeze);
        this.unfreezeLabel = context.getString(R.string.menu_unfreeze);
    }

    @Override
    public int getCount() {
        return items.size();
    }

    @Override
    public SavedItem getItem(int position) {
        return items.get(position);
    }

    @Override
    public long getItemId(int position) {
        return items.get(position).address;
    }

    @Override
    public View getView(int position, View convertView, ViewGroup parent) {
        ViewHolder h;
        if (convertView == null) {
            convertView = inflater.inflate(R.layout.row_saved, parent, false);
            h = new ViewHolder();
            h.cb = convertView.findViewById(R.id.cb_select);
            h.address = convertView.findViewById(R.id.tv_address);
            h.value = convertView.findViewById(R.id.tv_value);
            h.freeze = convertView.findViewById(R.id.btn_freeze);
            h.delete = convertView.findViewById(R.id.btn_delete);
            convertView.setTag(h);
        } else {
            h = (ViewHolder) convertView.getTag();
        }

        final SavedItem item = getItem(position);
        h.address.setText(item.addressHex());
        h.value.setText(item.value);
        h.cb.setChecked(item.selected);
        h.freeze.setText(item.frozen ? unfreezeLabel : freezeLabel);
        h.freeze.setBackgroundResource(item.frozen ? R.drawable.btn_primary : R.drawable.btn_dark);
        h.freeze.setTextColor(inflater.getContext().getColor(
                item.frozen ? R.color.on_primary : R.color.text));

        h.cb.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                item.selected = h.cb.isChecked();
            }
        });
        h.freeze.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                actions.onToggleFrozen(item);
            }
        });
        h.delete.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                actions.onDelete(item);
            }
        });
        convertView.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                actions.onEdit(item);
            }
        });
        return convertView;
    }

    private static class ViewHolder {
        CheckBox cb;
        TextView address;
        TextView value;
        Button freeze;
        ImageButton delete;
    }
}
