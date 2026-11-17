#ifndef MAPWORKER_H
#define MAPWORKER_H

/* ============================================================
   catsena —— worker / 命令队列 / 锁(并发骨架)
   设计: 内核只提供"命令队列 + 锁"的通用骨架, 并发模型由应用端
   启动时选定(max_workers=0 单线程串行 → 渐进到线程池)。
   配合 map.h 使用(map.h 已 include 本头)。
   ============================================================ */

#include "mapdef.h"

/* ---- 前向声明 ---- */
typedef struct map map;
#include "platform.h"   // map_thread_t(线程句柄类型, Win32 HANDLE / pthread_t)
typedef struct map_worker map_worker;
typedef struct map_engine map_engine;

/* ---- 配置(与 map.h 的 MapConfig 配合; 本结构是并发骨架部分) ---- */

typedef struct {
    int  max_workers;  /* 最大工作线程数; 0 = 单线程串行(命令即执行) */
    int  queue_depth;  /* 命令队列深度(0 = 不限) */
} MapOptions;

/* ---- 命令(队列项) ---- */

typedef struct map_cmd {
    char*  text;        /* 命令文本(自持) */
    char*  out;         /* 结果缓冲(调用方提供) */
    int    outsz;
    void (*done)(void* ud, int rc);  /* 完成回调(可 NULL) */
    void*  ud;          /* 回调上下文 */
    struct map_cmd* next;            /* 队列链 */
} map_cmd;

/* ---- 工作线程 ---- */

typedef struct map_worker {
    map*            map_instance;  /* 指向图实例 */
    int             worker_id;     /* 线程唯一标识 */
    void*           thread_ctx;    /* 线程局部(如 per-thread 节点缓存) */
    map_node*           current_node;  /* 当前处理节点(调试/统计) */
    bool            active;        /* 是否在跑命令 */
    void*           handle;        /* OS 线程句柄(实现细节) */
} map_worker;

/* ---- 引擎 ---- */

typedef struct map_engine {
    MapOptions options;      /* 配置 */
    map**       maps;         /* 图实例集合(可挂多图; 通常 1) */
    int         map_count;
    map_worker* workers;      /* 线程池(worker 数组) */
    int         worker_count; /* 实际 worker 数 */

    /* 命令队列(内核管理, 互斥保护) */
    map_cmd*    queue_head;
    map_cmd*    queue_tail;
    int         queue_len;
    void*       queue_lock;   /* 队列互斥(CRITICAL_SECTION*) */
    void*       queue_signal; /* 队列非空信号(CONDITION_VARIABLE*) */
    int         stopping;     /* 1 = 引擎正在关闭, worker 应退出 */
    map_thread_t* threads;     /* worker 线程句柄数组(map_thread_t) */
    int         thread_count; /* 已启动线程数 */

    /* 回收站(enable_trash 时生效): 延迟 free, 防读者悬垂 */
    struct map_trash_entry* trash;
    int   trash_count;
} map_engine;

/* 回收站条目(内核内部, 应用端只通过 map_gc 触发) */
struct map_trash_entry {
    char*   map_sha256;     /* 已删节点的指纹(调试/统计用) */
    map_node*   map_node;       /* 待释放节点 */
    uint64_t retired_at;/* 删除时刻(供延迟策略参考) */
    struct map_trash_entry* next;
};

/* ---- 引擎 API(实现按路线图渐进) ---- */

/* 创建引擎实例。返回新引擎; 失败 NULL。 */
map_engine* map_engine_create(const MapOptions* opts);

/* 销毁引擎: 停 worker、清队列、清 trash。 */
void map_engine_destroy(map_engine* e);

/* 挂载一个图实例(通常 1 个)。 */
int map_engine_attach_map(map_engine* e, map* m);

/* 提交命令到队列; 无空闲 worker 时排队。max_workers=0 时同步执行。 */
int map_engine_submit(map_engine* e, map* m, map_cmd* cmd);

/* 回收 trash(可手动或定时触发)。 */
int map_engine_gc(map_engine* e);

#endif /* MAPWORKER_H */
