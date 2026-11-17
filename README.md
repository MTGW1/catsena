# Catsena · 猫链

> 一个小型 C 图内核。节点，边，和一只不解释语义的猫。
> A small C graph kernel: nodes, edges, and a kernel that refuses to know what they mean.

Catsena 把「图」当成唯一的数据模型，除此之外什么都不理解 —— 没有页面、没有时间线、
没有权限、没有类型系统。它只认三件事：**节点、边、属性表**。所有业务语义留在应用层。

约 2900 行 C（不含内嵌 cJSON），零外部依赖（libc + 线程库），Windows / Linux 同一份源码。

---

## 为什么是它

**① 边也是节点。** 不是"另设一张边表"，边就是一个普通节点，只是两端集合非空。
于是"给关系挂属性"和"给节点挂属性"是同一件事，不存在第二套 API。

**② 两端都是集合。** 普通关系各 1 个端点；一对多 = A 端 1 个；**共享边 = 两端多个**。
`edge_end_add` / `edge_end_del` 幂等，某端摘空则边自动不再成立。

**③ uuid = 内容指纹。** 节点的 uuid 是"除 id/uuid 外全部字段"的 SHA256 ——
**同内容必然同 uuid**，去重、引用、缓存校验全部白送。
`id` 是持久句柄（内容变它不变），`uuid` 是内容指纹（内容变它就变），分工写死在 `DATA_MODEL.md §1`。

**④ 内核不解释 attrs。** `attrs` 是 `key=value` 表，内核只存不算。`role=page`、`status=done`、
`grant:proj=x` 是什么意思，全归应用层 —— 内核零感知，也零破坏。

**⑤ 锚定根（view）。** 同进程共享一份内存图，每个连接开一个 session 持有自己的锚定根，
路径从这个根解析。内核不解释"这根是什么意思"；应用层拿它表达"会话授权根"。

**⑥ 特调挂接点，不做特调。** 变更钩子（`on_change`）、可插拔索引、可插拔存档、
worker/命令队列骨架 —— 内核给挂接点，应用层挂自己那套。

---

## 快速开始

```sh
make            # 构建共享库 + 跑全部自测
make so         # 只构建 catsena.so / catsena.dll
make test       # 只跑自测
make info       # 打印 OS_KIND / LIB / 版本宏
```

依赖：C11 编译器；`version.h` 由 `tools/genversion.mjs` 从 `VERSION` + git 生成（需要 node，
但仓库里带了一份已生成的，所以**只用 C 编译器也能构建**）。

### 最小例子

```c
#include "map.h"

MapConfig cfg = {0};                 /* 全默认: 图级读写锁、无索引、无钩子 */
map* m = map_open("world.map", &cfg);/* 缺文件 = 空图 */
char out[4096];

map_exec(m, "create 角色A", out, sizeof out);
map_exec(m, "attr 角色A faction=设计", out, sizeof out);
map_exec(m, "link 角色A 角色B", out, sizeof out);

printf("%s\n", map_buildinfo(m));    /* catsena 0.1.0 (build …, git …) ABI 1 */
map_save(m, "world.map");
map_close(m);
```

从别的语言加载（Windows 的 `.dll` 带 `--export-all-symbols`，Linux 的 `.so` 默认导出）：

```python
import ctypes
lib = ctypes.CDLL("catsena.dll")
print(lib.map_version())             # b"0.1.0"
```

`_ffi_smoke.py` 是现成的 ctypes 烟雾测试。

---

## API 面（`map.h` / `API.md` 是唯一权威）

| 分组 | 内容 |
|---|---|
| 生命周期 | `map_open` `map_close` `map_save` |
| 版本 | `map_version` `map_build` `map_buildinfo` `map_abi` |
| 视图 | `map_root` `map_set_view` `map_view` |
| 会话 | `map_session_open` `map_session_close` `map_session_exec` `map_session_set_view` |
| 命令 | `map_exec`（同步） `map_submit`（入队，worker 线程） |
| 锁 | `map_lock_read/write` `map_unlock_read/write` |
| 遍历 | `map_find_uuid` `map_find_path` `map_collect_subtree` `map_gc` `map_emit` |
| node 级 | `map_node_new` `map_node_free` `node_is_edge` `link_parent_child` `edge_end_add/del` `node_attr_set/get` … |
| 底层 | `sha256`（内建，标准向量自测） |

内核内部函数不属于接口，变更不通知 —— 见 `API.md` 开头的契约声明。

---

## 自测

六个独立测试程序，`make test` 一次跑完：

| 测试 | 覆盖 |
|---|---|
| `maptest` | 命令面、视图锚定、并发命令队列、graphjson |
| `verify-sha256` | 9 组 SHA256 标准向量（空串 → 64/65 字节边界） |
| `test-id` | `id` 跨存档稳定、`uuid` 随内容变、还原后新建 id 不撞车 |
| `test-edge` | 1→N 边、N×M 共享边、端点摘空自删、旧格式存档兼容 |
| `test-mt` | 8 线程并发读写：堆完整 + 伸缩比 |
| `test-version` | 版本宏与 `map_*` 返回值一致性、ABI |

同一台机器实测（8 线程 × 800 次读写，Release `-O2`）：加速比 5.9–7.9x，伸缩效率 74–99%。

---

## 不做的事（边界）

| 不做 | 谁来干 |
|---|---|
| 权限/页面/时间线语义 | 应用层，识别 `attrs` 得出 |
| 归档轮转 / 备份策略 | 应用层（内核只保证存档原子写） |
| 自动保存（防抖） | 应用层（内核给 `on_change` 事件） |
| JSON 应用层数据 | 不需要 —— 命令行协议 + 自带存档格式 |
| 复杂查询 | 应用层注册为命令/回调，不进内核 |

---

## 目录

```
map.c map.h          图模型 + 全部导出 API（~1900 行）
mapdef.h             node / ptr_array / node_attr / data_kind
mapworker.c/.h       worker 线程池 + 命令队列骨架
platform.h           平台层（Windows crypto / POSIX 差异收口）
utils.c/.h           字符串/数组工具
version.h            ！生成物，别手改（tools/genversion.mjs）
thirdparty/cjson/    cJSON 1.7.19（MIT，见该目录 LICENSE）
maptest.c test-*.c verify-sha256.c  自测
API.md               内核↔应用接口契约（权威）
DEPENDENCIES.md      依赖清单与许可
```

---

## 名字

`catena`（拉丁语）＝**链**。文本校勘学里它还有个更窄的意思：
把**多位作者**的段落串到**同一条链**上，叫链注（catena）。
那正好是这个内核最特别的结构 —— **共享边**，一条边串起多个节点。

前面那个 `s` 是给猫的。血统来自一张自己的角色档案图（TheMap），也就是
`MAPFS.md` 里那套"把目录树掰成图、子权限也是边"的实验；
`AccessDenied.md` 那句「内核不知道什么是权限 —— 它只认边」就是这里的设计约束。

---

## 许可

Apache-2.0（见 `LICENSE`）。内嵌 cJSON 1.7.19 为 MIT，见 `thirdparty/cjson/LICENSE`。
