#!/usr/bin/env python3
"""从公开头文件生成 Windows 导出表 catsena.def。

为什么需要它：Windows 上 MinGW 默认用 `-Wl,--export-all-symbols`，那会把**所有**全局符号
都导进 DLL —— 包括内嵌 cJSON 的 83 个函数。库的使用者不该在自己的 ABI 里看到 cJSON，
而且将来若他们也链接一份 cJSON，会出现两个同名导出。

有 .def 之后, 导出面 = 公开头文件里声明的 map_* 函数, 一个不多一个不少。

用法:
    python tools/genexports.py          # 写 catsena.def
    python tools/genexports.py --check  # 只比对, 不一致时退出码 1(给 CI 用)
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADERS = ["map.h", "mapdef.h", "mapworker.h", "utils.h"]
OUT = os.path.join(HERE, "catsena.def")

# 去掉注释后再扫, 否则注释里的示例代码会被当成声明
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# 名字在行首或被类型前缀引导, 后接 '(' —— 并且整行不含 static(那是内部符号)
DECL = re.compile(r"^\s*(?!static)[A-Za-z_][\w\s\*]*?\b(map_[a-z0-9_]+)\s*\(", re.M)


def collect():
    names = set()
    for h in HEADERS:
        p = os.path.join(HERE, h)
        if not os.path.exists(p):
            continue
        text = open(p, encoding="utf-8").read()
        text = COMMENT.sub("", text)
        for m in DECL.finditer(text):
            names.add(m.group(1))
    return sorted(names)


def render(names):
    lines = ["; catsena.def —— 由 tools/genexports.py 从公开头文件生成, 别手改", "",
             "LIBRARY catsena.dll", "EXPORTS"]
    lines += ["    " + n for n in names]
    return "\n".join(lines) + "\n"


def main():
    names = collect()
    new = render(names)
    old = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
    if "--check" in sys.argv:
        if new == old:
            print("catsena.def 与头文件一致 (%d 个导出)" % len(names))
            return 0
        print("catsena.def 已过期, 跑 tools/genexports.py 重新生成")
        return 1
    open(OUT, "w", encoding="utf-8", newline="\n").write(new)
    print("catsena.def: %d 个导出" % len(names))
    return 0


if __name__ == "__main__":
    sys.exit(main())
