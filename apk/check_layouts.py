#!/usr/bin/env python3
# 扫描 apk 所有 layout，找出缺 layout_width/layout_height 的 View（排除 style 提供的情况）
import re, os, glob

ROOT = r'D:\project\workbuddy\工具\gg\apk'
styles_text = open(os.path.join(ROOT, 'res', 'values', 'styles.xml'), encoding='utf-8').read()

def style_has_w(sname):
    m = re.search(r'<style name="' + re.escape(sname) + r'"[^>]*>(.*?)</style>', styles_text, re.S)
    if not m:
        return False
    body = m.group(1)
    return ('android:layout_width' in body) and ('android:layout_height' in body)

# 收集多行标签：简单起见按 <Tag ... > 分段（自闭合或非自闭合都算）
VIEW_RE = re.compile(r'<(/?)([A-Za-z.]+)([^>]*?)(/?)>', re.S)

for f in sorted(glob.glob(os.path.join(ROOT, 'res', 'layout', '*.xml'))):
    text = open(f, encoding='utf-8').read()
    for m in VIEW_RE.finditer(text):
        closing, tag = m.group(1), m.group(2)
        if closing == '/':
            continue  # 跳过闭合标签 </...>
        if not re.search(r'View|Button|Text|Edit|Check|List|Scroll|Linear|Relative|Frame|Radio|Seek|Progress|Image|Spinner|Switch|Search|Rating|Chronometer|NumberPicker|AutoComplete|DatePicker|TimePicker|Calendar|Expandable|GridView|HorizontalScroll|Space|Web|Video|Surface|Texture|Flipper|Switcher|Include', tag):
            continue
        attrs = m.group(3)
        has_w = 'android:layout_width' in attrs
        has_h = 'android:layout_height' in attrs
        sm = re.search(r'style="@style/(\w+)"', attrs)
        style_ok = bool(sm) and style_has_w(sm.group(1))
        if not (has_w and has_h) and not style_ok:
            line = text[:m.start()].count('\n') + 1
            print(f'{os.path.basename(f)}:{line}: <{tag}> 缺 layout_width/height' + (f' (style={sm.group(1)})' if sm else ''))
