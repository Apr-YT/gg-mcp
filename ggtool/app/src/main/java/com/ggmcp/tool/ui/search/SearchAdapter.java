package com.ggmcp.tool.ui.search;

import android.content.Context;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.CheckBox;
import android.widget.TextView;

import com.ggmcp.tool.R;
import com.ggmcp.tool.model.SearchResult;
import com.ggmcp.tool.model.ValueType;

import java.util.ArrayList;
import java.util.List;

/** 搜索结果列表适配器（单击切换多选，长按由外部处理） */
public class SearchAdapter extends BaseAdapter {

    public interface OnSelectionChanged {
        void onChanged(int selectedCount);
    }

    private final LayoutInflater inflater;
    private final List<SearchResult> items = new ArrayList<>();
    private OnSelectionChanged selectionListener;

    public SearchAdapter(Context context) {
        this.inflater = LayoutInflater.from(context);
    }

    public void setSelectionListener(OnSelectionChanged l) {
        this.selectionListener = l;
    }

    public void setItems(List<SearchResult> list) {
        items.clear();
        if (list != null) items.addAll(list);
        notifyDataSetChanged();
    }

    public List<SearchResult> items() {
        return items;
    }

    public List<SearchResult> selected() {
        List<SearchResult> out = new ArrayList<>();
        for (SearchResult r : items) {
            if (r.selected) out.add(r);
        }
        return out;
    }

    public void setAllSelected(boolean sel) {
        for (SearchResult r : items) r.selected = sel;
        notifyDataSetChanged();
        notifySelection();
    }

    private void notifySelection() {
        if (selectionListener != null) selectionListener.onChanged(selected().size());
    }

    @Override
    public int getCount() {
        return items.size();
    }

    @Override
    public SearchResult getItem(int position) {
        return items.get(position);
    }

    @Override
    public long getItemId(int position) {
        return items.get(position).address;
    }

    /** GG 风格：类型语义着色 */
    private int typeColor(ValueType t) {
        switch (t) {
            case BYTE:   return inflater.getContext().getColor(R.color.type_byte);
            case WORD:   return inflater.getContext().getColor(R.color.type_word);
            case QWORD:  return inflater.getContext().getColor(R.color.type_qword);
            case FLOAT:  return inflater.getContext().getColor(R.color.type_float);
            case DOUBLE: return inflater.getContext().getColor(R.color.type_double);
            default:     return inflater.getContext().getColor(R.color.type_dword);
        }
    }

    @Override
    public View getView(final int position, View convertView, ViewGroup parent) {
        ViewHolder h;
        if (convertView == null) {
            convertView = inflater.inflate(R.layout.row_result, parent, false);
            h = new ViewHolder();
            h.cb = convertView.findViewById(R.id.cb_select);
            h.address = convertView.findViewById(R.id.tv_address);
            h.value = convertView.findViewById(R.id.tv_value);
            h.meta = convertView.findViewById(R.id.tv_meta);
            convertView.setTag(h);
        } else {
            h = (ViewHolder) convertView.getTag();
        }

        final SearchResult item = getItem(position);
        h.address.setText(item.addressHex());
        h.value.setText(item.value);
        h.meta.setText(item.region + " · " + item.type.label);
        // GG 风格：类型按语义着色
        h.meta.setTextColor(typeColor(item.type));
        h.cb.setChecked(item.selected);
        // GG 风格：选中行霓虹蓝高亮
        convertView.setBackgroundResource(item.selected ? R.drawable.row_selected_bg : R.drawable.row_bg);

        // 单击行 → 切换选中
        convertView.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                item.selected = !item.selected;
                h.cb.setChecked(item.selected);
                notifySelection();
            }
        });
        // CheckBox 自身点击同步（避免冒泡重复）
        h.cb.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                item.selected = h.cb.isChecked();
                notifySelection();
            }
        });
        return convertView;
    }

    private static class ViewHolder {
        CheckBox cb;
        TextView address;
        TextView value;
        TextView meta;
    }
}
