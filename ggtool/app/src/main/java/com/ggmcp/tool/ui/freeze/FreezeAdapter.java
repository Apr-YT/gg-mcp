package com.ggmcp.tool.ui.freeze;

import android.content.Context;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.TextView;

import com.ggmcp.tool.R;
import com.ggmcp.tool.model.FreezeItem;

import java.util.List;

/** 冻结列表适配器：启用/禁用、修改、删除 */
public class FreezeAdapter extends BaseAdapter {

    public interface Actions {
        void onToggle(FreezeItem item);
        void onEdit(FreezeItem item);
        void onDelete(FreezeItem item);
    }

    private final LayoutInflater inflater;
    private final List<FreezeItem> items;
    private final Actions actions;
    private final String frozenLabel;
    private final String activeLabel;

    public FreezeAdapter(Context context, List<FreezeItem> items, Actions actions) {
        this.inflater = LayoutInflater.from(context);
        this.items = items;
        this.actions = actions;
        this.frozenLabel = context.getString(R.string.status_frozen);
        this.activeLabel = context.getString(R.string.status_active);
    }

    @Override
    public int getCount() {
        return items.size();
    }

    @Override
    public FreezeItem getItem(int position) {
        return items.get(position);
    }

    @Override
    public long getItemId(int position) {
        return items.get(position).address;
    }

    @Override
    public View getView(final int position, View convertView, ViewGroup parent) {
        ViewHolder h;
        if (convertView == null) {
            convertView = inflater.inflate(R.layout.row_freeze, parent, false);
            h = new ViewHolder();
            h.dot = convertView.findViewById(R.id.v_status_dot);
            h.address = convertView.findViewById(R.id.tv_address);
            h.info = convertView.findViewById(R.id.tv_info);
            h.status = convertView.findViewById(R.id.tv_status);
            h.toggle = convertView.findViewById(R.id.btn_toggle);
            convertView.setTag(h);
        } else {
            h = (ViewHolder) convertView.getTag();
        }

        final FreezeItem item = getItem(position);
        h.address.setText(item.addressHex());
        h.info.setText("Value: " + item.value + "  Type: " + item.type.label);
        h.status.setText(item.enabled ? frozenLabel : activeLabel);
        h.status.setTextColor(inflater.getContext().getColor(
                item.enabled ? R.color.green : R.color.text_hint));
        h.dot.setBackgroundResource(item.enabled ? R.drawable.dot_green : R.drawable.dot_gray);
        h.toggle.setText(item.enabled ? frozenLabel : activeLabel);
        h.toggle.setBackgroundResource(item.enabled ? R.drawable.btn_primary : R.drawable.btn_dark);
        h.toggle.setTextColor(inflater.getContext().getColor(
                item.enabled ? R.color.on_primary : R.color.text));

        h.toggle.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                actions.onToggle(item);
            }
        });
        convertView.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                actions.onEdit(item);
            }
        });
        convertView.setOnLongClickListener(new View.OnLongClickListener() {
            @Override
            public boolean onLongClick(View v) {
                actions.onDelete(item);
                return true;
            }
        });
        return convertView;
    }

    private static class ViewHolder {
        View dot;
        TextView address;
        TextView info;
        TextView status;
        Button toggle;
    }
}
