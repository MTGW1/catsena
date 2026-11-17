#!/usr/bin/env python3
"""给 catsena 的公开符号补前缀（只补裸符号，map_* 保持不变）。

为什么不能用 sed/正则直接替换：
  1. `.map` 存档是 JSON, 键名里有 "node"/"nodes"/"edge"/"children" —— 改了存档就废了;
  2. 字符串字面量里有 ".map" 后缀、"map" 命令行词;
  3. 中文注释里到处是 node/map 这些英文词。
所以这里做一个最小的 C 词法扫描: 只在"普通代码上下文"里替换标识符,
字符串/字符字面量/行注释/块注释一律原样跳过。

用法:
    python tools/rename_symbols.py            # 干跑, 只打印将要改的位置
    python tools/rename_symbols.py --apply    # 真改
    python tools/rename_symbols.py --apply --include-comments   # 注释里也改(可选)
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 裸符号 -> 带前缀（按最长优先匹配，避免 node 抢在 node_attr 前面）
MAP = {
    # ---- 类型 ----
    "node": "map_node",
    "node_attr": "map_attr",
    "ptr_array": "map_ptr_array",
    "data_kind": "map_data_kind",
    "size_t_": "map_size",
    # ---- 枚举常量 ----
    "DATA_TEXT": "MAP_TEXT",
    "DATA_FILE": "MAP_FILE",
    # ---- 函数: node / link / edge 级 ----
    "node_compute_uuid": "map_node_uuid",
    "node_is_edge": "map_node_is_edge",
    "node_attr_set": "map_attr_set",
    "node_attr_get": "map_attr_get",
    "find_node_by_path": "map_find_node_by_path",
    "link_parent_child": "map_link_parent_child",
    "link_neighbor": "map_link_neighbor",
    "unlink_neighbor": "map_unlink_neighbor",
    "edge_connect": "map_edge_connect",
    "edge_connect_with": "map_edge_connect_with",
    "edge_end_add": "map_edge_end_add",
    "edge_end_del": "map_edge_end_del",
    "sha256": "map_sha256",
    # ---- 函数: worker 引擎 ----
    "engine_create": "map_engine_create",
    "engine_destroy": "map_engine_destroy",
    "engine_attach_map": "map_engine_attach_map",
    "engine_submit": "map_engine_submit",
    "engine_gc": "map_engine_gc",
    # ---- 函数: utils(原本是 PascalCase, 顺手变成 snake_case) ----
    "FindSelectedNodesWithOutsideConnections": "map_select_exposed_nodes",
    "IsNodesMayKnowEachOther": "map_nodes_may_know",
    "GetNodeRelatedUUIDs": "map_node_related_uuids",
}

CODE_EXT = (".c", ".h")
ID_START = re.compile(r"[A-Za-z_]")


def scan_replace(text, include_comments=False):
    """返回 (new_text, hits)。只在代码上下文替换标识符。"""
    out = []
    i, n = 0, len(text)
    hits = []
    while i < n:
        c = text[i]
        # 行注释
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            seg = text[i:j]
            if include_comments:
                for k, v in MAP.items():
                    seg = re.sub(r"\b%s\b" % re.escape(k), v, seg)
            out.append(seg)
            i = j
            continue
        # 块注释
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            seg = text[i:j]
            if include_comments:
                for k, v in MAP.items():
                    seg = re.sub(r"\b%s\b" % re.escape(k), v, seg)
            out.append(seg)
            i = j
            continue
        # 字符串 / 字符字面量(原样保留, 一行内的简单处理足够: C 里未闭合的会跨行但极少见)
        if c in "\"'":
            q = c
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == q:
                    j += 1
                    break
                if text[j] == "\n":
                    j += 1
                    break
                j += 1
            out.append(text[i:j])
            i = j
            continue
        # 标识符
        if ID_START.match(c):
            j = i + 1
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            word = text[i:j]
            if word in MAP:
                hits.append(word)
                out.append(MAP[word])
            else:
                out.append(word)
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out), hits


def main():
    apply = "--apply" in sys.argv
    include_comments = "--include-comments" in sys.argv
    total_files = total_hits = 0
    detail = {}
    for root, dirs, files in os.walk(HERE):
        dirs[:] = [d for d in dirs if d not in (".git", "thirdparty")]
        for fn in sorted(files):
            if not fn.endswith(CODE_EXT):
                continue
            p = os.path.join(root, fn)
            raw = open(p, "rb").read()
            bom = raw.startswith(b"\xef\xbb\xbf")
            text = raw.decode("utf-8-sig")
            new, hits = scan_replace(text, include_comments)
            if not hits:
                continue
            total_files += 1
            total_hits += len(hits)
            rel = os.path.relpath(p, HERE)
            from collections import Counter
            cnt = Counter(hits)
            detail[rel] = cnt
            print("%-16s %3d 处  %s" % (rel, len(hits),
                                        ", ".join("%s×%d" % kv for kv in cnt.most_common(6))))
            if apply:
                data = new.encode("utf-8")
                if bom:
                    data = b"\xef\xbb\xbf" + data
                open(p, "wb").write(data)
    print("\n%s: %d 个文件, %d 处" % ("已改" if apply else "将改", total_files, total_hits))
    if not apply:
        print("（干跑；加 --apply 才会写盘）")


if __name__ == "__main__":
    main()
