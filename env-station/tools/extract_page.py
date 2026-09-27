#!/usr/bin/env python3
"""extract_page.py — 把 app_web.c 里内嵌的 PAGE_HTML 抠成独立 .html（预检用）。
页面设计约定：HTML 属性全用单引号，C 字符串里无转义，直接拼接即可。"""
import io
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
s = io.open(src, encoding="utf-8").read()
m = re.search(r"static const char PAGE_HTML\[\] =\s*(.*?);\n", s, re.S)
if not m:
    sys.exit("PAGE_HTML not found")
parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
html = "".join(p.replace('\\"', '"') for p in parts)
io.open(dst, "w", encoding="utf-8", newline="\n").write(html)
print(f"segments={len(parts)} bytes={len(html)} -> {dst}")
