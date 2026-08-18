package com.ggmcp.tool.mock;

import com.ggmcp.tool.model.FreezeItem;
import com.ggmcp.tool.model.SavedItem;
import com.ggmcp.tool.model.SearchResult;

import java.util.ArrayList;
import java.util.List;

/**
 * 共享数据仓库：保存列表 + 冻结列表的单一数据源。
 * UI 各页面通过它增删改，注册监听刷新。
 */
public class DataStore {

    public interface Listener {
        void onSavedChanged();
        void onFrozenChanged();
    }

    private final List<SavedItem> saved = new ArrayList<>();
    private final List<FreezeItem> frozen = new ArrayList<>();
    private final List<Listener> listeners = new ArrayList<>();

    private static final DataStore INSTANCE = new DataStore();

    public static DataStore get() {
        return INSTANCE;
    }

    private DataStore() {
    }

    public void addListener(Listener l) {
        listeners.add(l);
    }

    public void removeListener(Listener l) {
        listeners.remove(l);
    }

    // ---- 保存列表 ----

    public List<SavedItem> saved() {
        return saved;
    }

    public void addToSaved(SearchResult r) {
        for (SavedItem s : saved) {
            if (s.address == r.address) {
                s.value = r.value;
                notifySaved();
                return;
            }
        }
        saved.add(new SavedItem(r.address, r.region, r.type, r.value));
        notifySaved();
    }

    public void addToSaved(SavedItem item) {
        for (SavedItem s : saved) {
            if (s.address == item.address) {
                s.value = item.value;
                notifySaved();
                return;
            }
        }
        saved.add(item);
        notifySaved();
    }

    public void updateSavedValue(SavedItem item, String newValue) {
        item.value = newValue;
        notifySaved();
    }

    public void removeSaved(SavedItem item) {
        saved.remove(item);
        // 同步移除其冻结项
        frozen.removeIf(f -> f.address == item.address);
        notifySaved();
        notifyFrozen();
    }

    public void removeSaved(long address) {
        saved.removeIf(s -> s.address == address);
        frozen.removeIf(f -> f.address == address);
        notifySaved();
        notifyFrozen();
    }

    /** 切换保存项冻结状态；冻结时同步加入冻结列表，解冻时移除 */
    public void toggleFrozen(SavedItem item) {
        item.frozen = !item.frozen;
        if (item.frozen) {
            boolean exists = false;
            for (FreezeItem f : frozen) {
                if (f.address == item.address) { exists = true; break; }
            }
            if (!exists) {
                frozen.add(new FreezeItem(item.address, item.type, item.value));
            }
        } else {
            frozen.removeIf(f -> f.address == item.address);
        }
        notifySaved();
        notifyFrozen();
    }

    // ---- 冻结列表 ----

    public List<FreezeItem> frozen() {
        return frozen;
    }

    public void setFrozenEnabled(FreezeItem item, boolean enabled) {
        item.enabled = enabled;
        notifyFrozen();
    }

    public void removeFrozen(FreezeItem item) {
        frozen.remove(item);
        // 同步保存列表解冻标志
        for (SavedItem s : saved) {
            if (s.address == item.address) {
                s.frozen = false;
            }
        }
        notifyFrozen();
        notifySaved();
    }

    public void updateFrozenValue(FreezeItem item, String newValue) {
        item.value = newValue;
        for (SavedItem s : saved) {
            if (s.address == item.address) {
                s.value = newValue;
            }
        }
        notifyFrozen();
        notifySaved();
    }

    // ---- 通知 ----

    private void notifySaved() {
        for (Listener l : listeners) l.onSavedChanged();
    }

    private void notifyFrozen() {
        for (Listener l : listeners) l.onFrozenChanged();
    }
}
