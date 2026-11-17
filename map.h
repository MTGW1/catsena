#ifndef MAP_H
#define MAP_H

/* ============================================================
   catsena —— TheMap 图内核(设计蓝图)
   定位: 纯图 + 并发骨架 + 特调挂接点。业务语义一律不进内核
   (见 THEMAP_ARCH.md §2.5 内核原则)。
   本头文件定义 API 面; 实现按路线图渐进(B: 共享图+命令队列 → A: 真并发)。
   ============================================================ */

#include "mapdef.h"
#include "mapworker.h"

/* ---- 引擎配置(应用端启动时选定, 内核按此装配) ---- */

typedef struct {
    MapOptions base;          /* 并发骨架配置(worker 数/队列深度, 见 mapworker.h) */
    int    enable_index;    /* 1 = 建 uuid 哈希索引(find 由 O(n) 变 O(1)) */
    int    enable_trash;    /* 1 = 删除进 trash 延迟回收(配合读锁) */
    int    lock_granularity;/* 0=图级读写锁; 1=节点级锁(懒创建, 开销大) */
    /* 特调挂接点(应用端可填 NULL 用默认):
       - on_change : 节点 建/改/删/链接 后回调(应用端做权限/审计/WS 广播)
       - index_fn  : 可插拔索引函数表(默认 uuid 哈希)
       - store_fn  : 可插拔存档函数表(默认 .map 聚合 JSON) */
    const struct map_hooks*   hooks;
    const struct map_index*   index_fn;
    const struct map_store*   store_fn;
} MapConfig;

/* ---- 变更事件钩子(应用端特调挂接点) ---- */

typedef enum {
    MAP_EVT_CREATE,   /* 节点创建     arg=map_node* */
    MAP_EVT_EDIT,     /* 字段编辑     arg=map_node* */
    MAP_EVT_ATTR,     /* 属性变更     arg=map_node* */
    MAP_EVT_LINK,     /* 链接建立     arg=map_node* (被链接节点) */
    MAP_EVT_UNLINK,   /* 链接断开     arg=map_node* */
    MAP_EVT_DELETE,   /* 节点删除     arg=map_node* (已摘除, 未释放) */
    MAP_EVT_SAVE,     /* 存档完成     arg=NULL */
} map_evt_kind;

/* 回调签名: 应用端注册, 内核在对应时机调用(已持锁, 回调内禁止再取锁)。
   返回非 0 可拒绝该操作(如权限校验失败 → 命令报错回滚)。 */
typedef int (*map_hook_fn)(map* m, map_evt_kind evt, map_node* arg, void* ud);

struct map_hooks {
    map_hook_fn on_change;   /* 通用变更(简化版: 所有事件都走它; 精细版看 evt) */
    void*       ud;          /* 回调上下文(应用端私有) */
};

/* ---- 可插拔索引(应用端按需挂接) ---- */

struct map_index {
    /* 建立/维护索引: 节点增删改时内核调用 */
    int  (*upsert)(map* m, map_node* n);
    int  (*remove)(map* m, map_node* n);
    /* 查询: 命中返回节点指针(内核不接管所有权) */
    map_node* (*find_by_uuid)(map* m, const char* uuid);
    map_node* (*find_by_path)(map* m, const char* path);
    /* 释放索引 */
    void (*destroy)(map* m);
};

/* ---- 可插拔存档(默认 .map 聚合 JSON) ---- */

struct map_store {
    int  (*save)(map* m, const char* path);   /* 全量/增量存档, 返回 0 成功 */
    map* (*load)(map* m, const char* path);   /* 载入到已开实例 */
};

/* ---- 图实例 ---- */

/* map 实例结构(命名结构, 与 mapworker.h 前向声明 typedef struct map map; 一致)。
   内核管理生命周期; 应用端可经 map_* API 访问, 也可直读只读字段。 */
typedef struct map {
    map_node  self;          /* 图自身(容器节点) */
    map_node* root;          /* 根节点(项目根) */
    void* all_nodes;     /* 所有节点的集合(哈希表/其他结构, 内核或应用端挂接) */
    void* index;         /* 节点索引(可插拔: map_index 函数表 + 此句柄) */
    void* lock_nodes;    /* 节点锁(多线程节点访问控制; lock_granularity=1 时启用) */
    /* 过程缓存 */
    void* trash;         /* 垃圾回收(已删节点延迟释放; enable_trash 时启用) */
    int   trash_count;   /* 当前回收站资源数 */
    void* last_accessed; /* 最近访问节点(缓存/优化) */
    void* last_opt;      /* 最近操作(调试/统计) */

    /* 装配(内核填, 由 map_open 按 MapConfig 设置) */
    MapConfig        config;    /* 本实例配置(含挂接点) */
    map_engine*      engine;    /* 所属引擎(map_engine_attach_map 后非 NULL) */
    void*            lock;      /* 图级读写锁(SRWLock*) */
    void*            app_ctx;   /* 内部实现细节: 旧内核 map_app*(load_map/create_map 结果) */
    map_node*            view_root; /* 视图锚定根(NULL=整图; 应用端权限语义用) */
} map;

/* 打开(或新建)图实例: 载入 .map(缺省则空图), 按 config 装配索引/钩子/worker。
   返回新实例; 失败 NULL。 */
map* map_open(const char* path, const MapConfig* config);

/* ---- 版本 ----
   版本号写在 VERSION 文件里, 编译前由 tools/genversion.mjs 生成 version.h。
   上层(服务层/控制台)靠这四个函数问内核"你是谁、什么版本", 不要各处硬编码。 */
const char* map_version(void);     /* "0.1.0" */
const char* map_build(void);       /* "0.1.0+20261102.153014.1142cd60" */
const char* map_buildinfo(void);   /* "catsena 0.1.0 (build …, git …) ABI 1" */
int         map_abi(void);         /* 1 —— 接口不兼容地变了才加 */

/* 关闭实例: 停止 worker, 回收索引/trash, 释放所有节点。 */
void map_close(map* m);

/* 根节点(图自身容器, data=图名)。 */
map_node* map_root(map* m);

/* 视图锚定根(纯图能力): 把命令路径解析锚定到指定节点(如项目子树/授权根)。
   NULL = 整图(默认)。应用端权限语义用它表达"会话授权根", 内核不解释含义。 */
int map_set_view(map* m, map_node* view_root);
/* 当前锚定根(NULL=整图)。 */
map_node* map_view(map* m);

/* ---- 会话视图(多连接共享同一 map 实例的标准方式) ----
   map 实例是进程共享的(一份内存图, 读写锁保护);
   每个连接开一个会话, 会话持有自己的锚定根 —— 互不干扰。
   应用端: main 打开一份 map, 每 WS 连接 map_session_open + attach 设锚定根。 */
typedef struct map_session map_session;
map_session* map_session_open(map* m);
void map_session_close(map_session* s);
/* 会话内执行命令(路径从会话锚定根解析; 锁与 map_exec 相同) */
int map_session_exec(map_session* s, const char* cmd, char* out, int outsz);
/* 会话锚定根(按路径, 相对 map 整图 root; "" = 整图) */
int map_session_set_view(map_session* s, const char* path);

/* ---- 命令执行(与 ui_exec_ctx 同语义: attach/ls/create/... 全走这里) ---- */

/* 同步执行一条命令。内部: 取写锁/读锁(按命令类型) → 执行 → 触发钩子 → 释放锁。
   返回写入 out 的字节数; <0 失败。 */
int map_exec(map* m, const char* cmd, char* out, int outsz);

/* 异步提交: 入命令队列, 由 worker 线程执行; 完成写 out 并唤醒回调(可 NULL)。
   max_workers=0 时退化为同步执行(等同 map_exec)。 */
int map_submit(map* m, const char* cmd, char* out, int outsz,
               void (*done)(void* ud, int rc), void* ud);

/* ---- 锁(内核提供, 应用端特调时也可手动取) ---- */

/* 读锁(查询类命令用): 允许多读者; 返回 0 成功。 */
int map_lock_read(map* m);
/* 写锁(编辑/存档类命令用): 排他; 返回 0 成功。 */
int map_lock_write(map* m);
/* 释放读锁 / 释放写锁(必须与获取类型匹配 —— 共享状态不可跨线程覆盖) */
int map_unlock_read(map* m);
int map_unlock_write(map* m);

/* ---- 查询(内核基础遍历; 复杂查询由应用端用钩子/命令挂接) ---- */

/* 按 uuid 精确查找(走 index_fn 或默认遍历)。 */
map_node* map_find_uuid(map* m, const char* uuid);
/* 按路径从 root 解析(走 index_fn 或默认遍历)。 */
map_node* map_find_path(map* m, const char* path);
/* 子树收集: 沿 child 链收集到 arr(上限 cap), 返回数量。 */
map_size map_collect_subtree(map* m, map_node* root, map_node** arr, map_size cap);

/* ---- 垃圾回收(enable_trash 时生效) ---- */

/* 手动触发 trash 回收(删除后延迟 free 的节点真正释放)。 */
int map_gc(map* m);

/* ---- 变更通知辅助(应用端可直接调用触发钩子) ---- */

int map_emit(map* m, map_evt_kind evt, map_node* arg);

/* ---- map_node 级 API(图操作底层, 命令层 map_exec 的原子件) ---- */

/* 建节点(data 外部所有, owns_data=0; TEXT 自动 strlen; 自动算 map_sha256+uuid) */
map_node* map_node_new(map_data_kind dtype, void* data, map_size data_len);
/* 声明某个 id 已被占用(载入存档还原 id 之后必调; 保证新建节点 id 不撞车)。
   id = 持久句柄(内容变不变, 变), uuid = 内容指纹; 分工见 DATA_MODEL.md §1。 */
void  map_node_reserve_id(uint64_t used_id);
/* 计算/重算 uuid(除 id/uuid 外字段指纹, 含 attrs; 调用方接管) */
char* map_node_uuid(const map_node* n);
/* 释放节点(摘除全部关系 + 递归释放子树) */
void map_node_free(map_node* n);
/* 边角色判定(内容判定式): 两端集合都非空即边(载荷类型与角色正交) */
int  map_node_is_edge(const map_node* n);
/* 双向父子链接 / 无向邻接(断开) / 边节点 */
int  map_link_parent_child(map_node* parent, map_node* child);
int  map_link_neighbor(map_node* a, map_node* b);
int  map_unlink_neighbor(map_node* a, map_node* b);
map_node* map_edge_connect(map_node* a, map_node* b);                              /* 空边(载荷 NULL) */
map_node* map_edge_connect_with(map_node* a, map_node* b, void* data, size_t len); /* 带载荷边(data 外部所有) */
/* 边端点集合: 边两端都是集合(1→N / 共享边同一结构)。
   side: 0 = 主体(A, 发出侧) / 1 = 客体(B, 指向侧)。add 幂等, del 返回 1 = 原本不在。 */
int  map_edge_end_add(map_node* e, map_node* n, int side);
int  map_edge_end_del(map_node* e, map_node* n, int side);
/* 属性读写(值 NULL/空串 = 删; 变更重算 uuid) */
int  map_attr_set(map_node* n, const char* key, const char* value);
const char* map_attr_get(const map_node* n, const char* key);
/* 按路径从 root 解析("" = root; "a/b/c") */
map_node* map_find_node_by_path(map_node* root, const char* path);
/* 存档(明文 JSON, 格式兼容旧 .map) */
int map_save(map* m, const char* path);

int map_sha256(const char* data, size_t len, char* out_hex);

#endif /* MAP_H */
