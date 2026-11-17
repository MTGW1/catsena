#!/usr/bin/env python3
"""把 C 源码/头文件**注释里**的旧符号名对齐到 rename_symbols 的映射。

代码文件改名时故意跳过注释（避免动到字符串里的 JSON 键）；但公开头文件的注释本身就是
API 文档，索引 —— 这个工具专门补那一步。

用法:
    python tools/align_comments.py           # 干跑
    python tools/align_comments.py --apply
"""
import os
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HERE, "tools"))
from rename_symbols import scan_replace  # noqa: E402

TARGETS = ["map.h", "mapdef.h", "mapworker.h", "platform.h", "utils.h",
           "map.c", "mapworker.c", "utils.c"]


def main():
    apply = "--apply" in sys.argv
    total = 0
    for fn in TARGETS:
        p = os.path.join(HERE, fn)
        if not os.path.exists(p):
            continue
        text = open(p, encoding="utf-8", newline="").read()
        new, hits = scan_replace(text, include_comments=True)
        if not hits:
            continue
        total += len(hits)
        print("%-14s %4d 处" % (fn, len(hits)))
        if apply:
            open(p, "w", encoding="utf-8", newline="").write(new)
    print("\n%s: %d 处" % ("已改" if apply else "将改（加 --apply 写盘）", total))


if __name__ == "__main__":
    main()
