#!/usr/bin/env python3
"""把文档(md)里的符号名对齐到改名后的代码。

只改两处, 其余原样:
  1. ``` 围栏代码块内部 —— 当作 C 代码, 用 rename_symbols 的同一张映射表;
  2. 行内 `反引号` 里的标识符。
正文散文一律不动(否则 "node" 这种英文词会把说明文改烂)。

另外保护: verify-sha256 是**文件名**不是符号, 不能被改成 verify-map_sha256。

用法: python tools/rename_docs.py [--apply]
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rename_symbols import MAP, scan_replace  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROTECT = ["verify-sha256"]          # 文件名等不可动的字面量

DOCS = ["API.md", "README.md", "DEPENDENCIES.md"]

FENCE = re.compile(r"^```.*?^```", re.S | re.M)
TICK = re.compile(r"`[^`\n]+`")


def protect(s):
    for k, i in zip(PROTECT, range(len(PROTECT))):
        s = s.replace(k, "\x00P%d\x00" % i)
    return s


def unprotect(s):
    for k, i in zip(PROTECT, range(len(PROTECT))):
        s = s.replace("\x00P%d\x00" % i, k)
    return s


def inline_fix(s):
    """反引号内的标识符: 用正则按词替换(反引号里不会有字符串字面量)"""
    def one(m):
        body = m.group(0)[1:-1]
        for k, v in sorted(MAP.items(), key=lambda kv: -len(kv[0])):
            body = re.sub(r"\b%s\b" % re.escape(k), v, body)
        return "`" + body + "`"
    return TICK.sub(one, s)


def main():
    apply = "--apply" in sys.argv
    total = 0
    for name in DOCS:
        p = os.path.join(HERE, name)
        if not os.path.exists(p):
            continue
        text = open(p, encoding="utf-8").read()
        text = protect(text)

        def fence_fix(m):
            block = m.group(0)
            lines = block.split("\n")
            # 首尾是 ``` 行, 中间按代码处理
            body = "\n".join(lines[1:-1])
            new, hits = scan_replace(body)
            return "\n".join([lines[0], new, lines[-1]]) if hits else block

        new = FENCE.sub(fence_fix, text)
        new = inline_fix(new)
        new = unprotect(new)
        if new != unprotect(text):
            total += 1
            print("%-16s 有变化" % name)
            if apply:
                open(p, "w", encoding="utf-8", newline="").write(new)
    print("\n%s: %d 个文档" % ("已改" if apply else "将改", total))
    if not apply:
        print("（干跑；加 --apply 才写盘）")


if __name__ == "__main__":
    main()
