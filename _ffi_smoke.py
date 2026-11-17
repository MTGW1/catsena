#!/usr/bin/env python3
"""smoke test: load catsena.so through ctypes (same path koffi takes in ffi.mjs)

契约(踩过): map_session_exec 返回的是**输出字节数**(>=0), 负值才是错误。
            不是"0 = 成功"。ffi.mjs 用它当长度切 buffer。
契约(踩过): 任何写命令都会改 uuid -> 写之后必须重新 graphjson, 不能用旧 uuid。
"""
import ctypes, json, os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

def find_lib():
    """库路径查找顺序: argv[1] > $CATSENA_LIB > 脚本同级目录(catsena.so/.dylib/.dll)"""
    cands = []
    if len(sys.argv) > 1:
        cands.append(sys.argv[1])
    if os.environ.get("CATSENA_LIB"):
        cands.append(os.environ["CATSENA_LIB"])
    for name in ("catsena.so", "libcatsena.so", "catsena.dylib", "catsena.dll"):
        cands.append(os.path.join(HERE, name))
    for c in cands:
        if os.path.exists(c):
            return c
    sys.exit("找不到内核库; 用法: python _ffi_smoke.py <catsena.so|.dll>  或设 CATSENA_LIB")

SO = find_lib()
print("lib:", SO)
lib = ctypes.CDLL(SO)
lib.map_open.restype = ctypes.c_void_p
lib.map_open.argtypes = [ctypes.c_char_p, ctypes.c_void_p]
lib.map_session_open.restype = ctypes.c_void_p
lib.map_session_open.argtypes = [ctypes.c_void_p]
lib.map_session_exec.restype = ctypes.c_int
lib.map_session_exec.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
lib.map_save.restype = ctypes.c_int
lib.map_save.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
lib.map_close.restype = None
lib.map_close.argtypes = [ctypes.c_void_p]

ok = fail = 0
def check(cond, msg):
    global ok, fail
    if cond: ok += 1;  print("  ok   " + msg)
    else:    fail += 1; print("  FAIL " + msg)

path = os.path.join(tempfile.gettempdir(), "catsena-smoke.map")
if os.path.exists(path):
    os.remove(path)

m = lib.map_open(path.encode(), None)
check(bool(m), "map_open (empty graph)")
sess = lib.map_session_open(m)
check(bool(sess), "map_session_open")

def run(cmd, expect_len=True):
    buf = ctypes.create_string_buffer(1 << 20)
    n = lib.map_session_exec(sess, cmd.encode(), buf, len(buf))
    return n, buf.value.decode("utf-8", "replace")

def graph():
    n, s = run("graphjson")
    return json.loads(s[s.index("{"):]) if n > 0 else {}

n, out = run('create "" ProjectAlpha')
check(n > 0 and "已建" in out, "create #1 -> %r (n=%d)" % (out.strip()[:40], n))
n, out = run('create "" ProjectBeta')
check(n > 0 and "已建" in out, "create #2 -> %r" % out.strip()[:40])

g = graph()
nodes = g.get("nodes", [])
check(len(nodes) >= 3, "graphjson -> %d nodes" % len(nodes))
check(any(x.get("name") == "ProjectAlpha" for x in nodes), "UTF-8 name round-trips through JSON")

u = [x["uuid"] for x in nodes if x.get("name")]           # 有名字的节点
check(len(u) >= 2, "got uuids for edge test")
if len(u) >= 2:
    n, out = run("edge uuid:%s uuid:%s rel_test" % (u[0], u[1]))
    check(n > 0 and "已建" in out, "edge_connect -> %r" % out.strip()[:40])

    # 写会换 uuid -> 重新取
    g2 = graph()
    ed = [x for x in g2.get("nodes", []) if x.get("edge")]
    check(len(ed) >= 1, "edge node present after write (%d)" % len(ed))
    if ed:
        eu = ed[0]["uuid"]
        n, out = run("elink uuid:%s uuid:%s b +" % (eu, ed[0].get("edge_b_uuid") or u[1]))
        check(n > 0 and "A=" in out, "elink add end -> %r" % out.strip()[:50])

n = lib.map_save(m, path.encode())
check(n == 0, "map_save returns 0")
check(os.path.exists(path) and os.path.getsize(path) > 200,
      "save file exists (%d bytes)" % (os.path.getsize(path) if os.path.exists(path) else 0))

lib.map_close(m)
print("\n== %s (%d ok, %d fail) ==" % ("PASS" if fail == 0 else "FAIL", ok, fail))
sys.exit(0 if fail == 0 else 1)
