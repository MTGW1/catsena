# catsena 内核↔应用接口契约

> 版本 1.0 (2026-08-23)。本文档是 catsena 与应用层之间的**唯一权威接口**。
> 应用层(Python/Node/其他)只通过这里列出的 API/命令与内核交互;
> 内核内部结构(map.c 内部函数)不属于接口, 变更不通知。
> **布局: 数据模型(节点定义)置顶 —— 接口从"数据长什么样"开始。**

## 1. 数据模型: 节点定义(mapdef.h 原文)

```c
typedef struct map_node {
    uint64_t     id;
    map_data_kind    dtype;      /* 载荷类型(TEXT/FILE) */
    void*        data;       /* 不透明载荷 */
    unsigned     owns_data : 1; /* data 是否由节点自持: 1=destroy 时 free */
    unsigned     alive : 1;     /* 1=活跃; DAG(多父/共享)下防二次释放 */
    map_size      data_len;   /* data 字节长度(TEXT=strlen) */
    char*        map_sha256;     /* data 的 SHA256, hex(可为 NULL: 二进制未知) */

    /* 关系(指针集合) */
    map_ptr_array neighbors;   /* 邻节点(无向) */
    map_ptr_array childs;      /* 子节点(有向被指向端) */
    map_ptr_array parents;     /* 父节点(有向指向端) */
    map_ptr_array edges;       /* 边节点列表 */
    map_ptr_array edge_as;     /* 边节点专用: 主体端(集合, 1→N / 共享边同一结构) */
    map_ptr_array edge_bs;     /* 边节点专用: 客体端(集合) */

    /* 属性表(node_attr*) —— 应用端语义识别 */
    map_ptr_array attrs;

    char*    uuid;       /* 唯一标识 = 除 id/uuid 外字段的 SHA256 指纹 */
} map_node;
```

**解读(应用层必读)**:
- 节点 = 数据容器 + 关系端点二合一; 边也是节点(两端集合都非空即判定, 见 `map_node_is_edge`)
- **边两端都是集合**: 普通关系各 1 个; 一对多 = A 端 1 个; 共享边 = 两端多个。
  加/减端点用 `map_edge_end_add` / `map_edge_end_del`(幂等, 内部会刷 uuid); 某端摘空则边不再成立
- `attrs` = 应用层的语义出口: 内核只存, key 含义全部由应用端约定(角色/权限/时间线/页面)
- `uuid` = 内容寻址指纹: **同内容(uuid 字段集同)则同 uuid**, 可用于去重/引用
- `data` 不透明: TEXT 字符串 / FILE 路径或二进制, 与角色正交

### 支撑类型(简明版, 完整见 mapdef.h)
```c
typedef struct map_ptr_array { void** items; map_size count, capacity; } map_ptr_array;
typedef struct map_attr { char* key; char* value; } map_attr;   // 应用端语义键
typedef enum { MAP_TEXT, MAP_FILE } map_data_kind;                  // 载荷类型(不含角色)
```

## 2. 分层边界

```
应用层(你写的)                 内核(catsena, 只做图)
─────────────────────────────────────────────────────
权限/页面/时间线语义         →   不感知(attrs 只存不解释)
归档/备份/版本(.bak 轮转)    →   不做(只保证原子写)
自动保存(防抖触发)           →   不做(提供 on_change 事件)
JSON 应用层数据              →   不需要(命令行协议, 内核自带存档用 cJSON)
```

## 3. C API(绑定层/嵌入式使用)

### 版本(编译期写死, 上层别再硬编码)
```c
const char* map_version(void);     // "0.1.0"           ← VERSION 文件
const char* map_build(void);       // "0.1.0+20261102.153014.1142cd60"
const char* map_buildinfo(void);   // "catsena 0.1.0 (build …, git …) ABI 1"
int         map_abi(void);         // 1                 ← 接口不兼容地变了才加
```

版本链路(改版本只改一处):

```
catsena/VERSION           ← 唯一真相(语义化版本)
   ↓  make (= version.h 规则)
tools/genversion.mjs  →  version.h   (自动生成: 版本/build/日期/git/BUILD_ID/ABI)
   ↓  编译进 map.c
map_version() / map_build() / map_buildinfo() / map_abi()
   ↓  服务层绑定(koffi)
WS `version` 命令 + `GET /version`  →  控制台欢迎信息 / uname / ver
```

- `version.h` 里带 `git describe --always --dirty`(不在仓库里就是 `nogit`),
  所以"跑的是哪次构建"一眼能看出来(比如 `bcbfa4d-dirty` 说明工作区有未提交改动)。
- 内容没变时 `genversion.mjs` **不重写文件**, 避免每次 make 全量重编。
- `make version` 只打印不落盘; `make clean-version` 删掉让它下次重新生成。
- 自测: `make test` 里的 `test-version` 校验四个函数非空 / 版本一致 / build 前缀 / ABI。

### 生命周期
```c
map* map_open(const char* path, const MapConfig* cfg);  // 载入或新建(不存在=空图)
void map_close(map* m);
map_node* map_root(map* m);
int  map_set_view(map* m, map_node* view_root);   // 锚定根(整图= NULL)
map_node* map_view(map* m);
```

### 会话(多连接标准方式: 共享 map 实例, 每连接一个会话)
```c
map_session* map_session_open(map* m);
void         map_session_close(map_session* s);
int  map_session_exec(map_session* s, const char* cmd, char* out, int outsz);
int  map_session_set_view(map_session* s, const char* path);  // "" = 整图
```

### 命令执行
```c
int map_exec(map* m, const char* cmd, char* out, int outsz);   // map 级(锚定=view_root 或整图)
int map_submit(map* m, ..., done_cb, ud);                      // 异步(engine 挂接时)
```

> **返回值契约（容易读错）**：`map_exec` / `map_session_exec` 返回的是**写进 `out` 的字节数**
> （语义同 `snprintf`），**不是 0=成功**。`>= 0` 才能 `out[0..n)` 取文本，负值才是错误。
> `ffi.mjs` 正是拿它当长度切 Buffer 的 —— 按"0 才是成功"去判会得出"全失败"的错误结论（踩过）。

### 查询(直接 C 用; 应用层可只走命令)
```c
map_node* map_find_uuid(map* m, const char* uuid);
map_node* map_find_path(map* m, const char* path);
map_size map_collect_subtree(map* m, map_node* root, map_node** arr, map_size cap);
```

### 图操作(node 级, 原子件)
```c
map_node* map_node_new(map_data_kind, void* data, map_size len);   // TEXT 自动 strlen
void  map_node_free(map_node* n);
int   map_link_parent_child(map_node* parent, map_node* child);
int   map_link_neighbor(map_node* a, map_node* b);
int   map_unlink_neighbor(map_node* a, map_node* b);
map_node* map_edge_connect(map_node* a, map_node* b);                            // 空边
map_node* map_edge_connect_with(map_node* a, map_node* b, void* data, map_size l); // 带载荷边
int   map_edge_end_add(map_node* e, map_node* n, int side);                   // 加端点(幂等) side: 0=A 1=B
int   map_edge_end_del(map_node* e, map_node* n, int side);                   // 减端点, 返回 1=原本不在
int   map_node_is_edge(const map_node* n);                                // 结构判定
int   map_attr_set(map_node* n, const char* key, const char* value); // NULL/空=删
const char* map_attr_get(const map_node* n, const char* key);
```

### 存档
```c
int map_save(map* m, const char* path);   // 原子写: 先 .tmp 后 rename, 防半文件
int map_load(map* m, const char* path);   // 向已有实例载入(替换 root)【v1.0 新增公开】
```

### 并发/钩子
```c
map_lock_read/write + map_unlock_read/write;   // 图级 SRWLock/pthread_rwlock
map_engine* map_engine_create(opt); attach_map / submit / destroy;  // worker 线程池
int map_emit(map* m, evt, map_node* arg);          // on_change 钩子触发点
```
`MapConfig`: `base.max_workers`(线程池大小) / `enable_index` / `enable_trash` /
`hooks->on_change`(应用端挂: 变更→自动保存/广播)、`index_fn`/`store_fn`(可插拔)。

### 图查询辅助(utils.h，建在图操作之上的常用问答)

```c
void map_uuids_free(char** uuids, size_t count);   // 下面所有 char** 输出的唯一正确出口

// 1. 框选分析: 选区内的节点若有一条关系连到选区外 → 它是"边界节点"
int map_select_exposed_nodes(map* m, const map_node** selected, size_t n,
                             char*** out_uuids,          size_t* out_count,        // 边界节点
                             char*** out_outside_uuids,  size_t* out_outside_count); // 选区外邻居(去重)

// 2. 两点是否同一子图(沿 邻/父/子/边 + 边两端集合 做无向连通) → 1/0/-1
int map_nodes_may_know(map* m, const map_node* a, const map_node* b);

// 3. 周围有谁(去重): mode 是位掩码, 不是 0/1/2 枚举
int map_node_related_uuids(const map_node* n, int mode, char*** out, size_t* count);
//   MAP_REL_NEIGHBOR(1) | MAP_REL_CHILD(2) | MAP_REL_PARENT(4) ; MAP_REL_ALL = 7
```

约定与坑(全部由 `test-utils` 钉死):

- **所有权**: 所有 `char**` 都由内核分配, 必须用 `map_uuids_free()`, 不要自己 `free(arr)`。
- **失败安全**: 参数错误时返回 -1 且四个输出全部被置成 NULL/0, 失败后照样可以安全释放。
- **纯查询**: 三个函数都不取锁、不改图; 调用方需自己保证并发期间图不被改(或在外层持读锁)。
- **`may_know` 的语义边界**: 树状结构(所有节点都在同一 root 下)里任意两点都连通 ——
  这个函数只在存在独立连通分量时才有区分度。
- **`mode` 是位掩码**: 早期文档写成 0=邻居/1=子/2=父 的枚举, 和实现对不上; 现在以位掩码为准,
  `mode=0` 或越界一律返回 -1(不静默返回空数组)。
- **边节点也要选进去**: 框选时若漏掉边节点, 它会被算作"选区外", 导致边界节点和外部列表都偏大。

## 2. 命令协议(应用层主要通道)

文本命令 → `map_session_exec` → 文本回执。路径锚定会话根; `uuid:<hex>` 直接寻址。

| 命令 | 行为 | 示例 |
|---|---|---|
| `create <parent> <name>` | 建 TEXT 子节点(`""`=锚根) | `create root 桃芊` |
| `set <path> <新data>` | 改载荷(整行为值) | `set uuid:xx 描述...` |
| `attr <path> <key> [v\|-]` | 属性 读/设/删 | `attr 桃芊 relation 姐妹` |
| `del <path>` | 删节点(含子树) | `del root/桃芊` |
| `neighbor <a> <b>` | 无向邻接 | `neighbor 桃芊 桃香` |
| `edge <a> <b> [内容]` | 边(空边=邻级, 带内容=跨级) | `edge 桃芊 桃香 恋人` |
| `elink <边> <节点> <a\|b> [+\|-]` | 给边加减端点 → 一对多 / 共享边 | `elink uuid:e uuid:n b +` |
| `get <path>` | 读载荷原文 | `get uuid:xx` |
| `path <path>` | 路径→uuid | `path root/桃芊` |
| `ls [path]` | 子树打印 | |
| `graphjson` | 图导出(nodes/links; 含 `edge`/`page` 标记) | |
| `save [path]` | 原子存档(整图) | |
| `help` | 命令列表 | |

**回执约定**: 成功含结果/`rc=0`; 失败简洁错误(`xxx: 原因`)。`graphjson` 回 JSON 文本。
**转义**: 多行数据(set/page 载荷)用 `\n \t \\ \"` 单行转义(应用层负责, 内核原样存取)。

## 3. 数据约定

- **路径**: `/` 分隔, 空串=锚定根, 段匹配节点 data(TEXT); `uuid:<64hex>` 绕过名字。
- **锚定**: 会话 `view_root` 限定一切命令作用域(权限收缩点, 应用层 attach 后设置)。
- **边角色**: `map_node_is_edge` = 两端集合都非空(结构判定, 载荷类型随意)。
- **uuid**: 内容寻址指纹; attrs 非空才计入(旧档兼容); load 保留原 uuid。
- **隐藏语义**: `attrs.hidden=1` → graphjson `sys:1`(前端过滤; 旧档 reserved 自动迁移)。
- **存档格式**: 明文 JSON(嵌套 children + 顶层 edges), 兼容旧 .map。
- **并发语义**: 读命令共享锁, 写命令排他; 命令级自动(无需手工加锁)。

## 4. on_change 事件(应用端主挂点)

```c
map_evt_kind: MAP_EVT_CREATE/EDIT/ATTR/LINK/UNLINK/DELETE/SAVE
int (*on_change)(map* m, map_evt_kind evt, map_node* arg, void* ud);
```
- 命令执行后粗粒度触发(当前: `MAP_EVT_SAVE` 后统一通知)。
- 应用端典型用途: 防抖自动保存、WS 广播给其他客户端、审计日志。

## 5. 应用层职责(内核不做)

| 职责 | 说明 |
|---|---|
| 归档/备份 | `.bak` 轮转/版本目录(用户侧, 内核 save 已原子写防半文件) |
| 自动保存 | 监听 on_change/命令 → 防抖调 `save` |
| 语义识别 | attrs 的 key 含义: page/axis/grant/tag/relation... (见 THEMAP_ARCH.md 字典) |
| 权限 | attach/projects/grant 逻辑(解析 _acl 子树 → 会话锚定) |
| 界面 | 图渲染/页面渲染/泳道等全部前端 |

## 6. 扩展位(预留)

`MapConfig.enable_trash`(延迟回收)/ `index_fn`(替换索引)/ `store_fn`(自定义存档器)/
`map_engine_submit` 的 `queue_depth` 流控 —— 均为**预留空位**, 实现待补。
