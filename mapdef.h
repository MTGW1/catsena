#ifndef MAPDEF_H
#define MAPDEF_H

/* ============================================================
   catsena —— 独立图内核类型定义(不依赖旧内核 coredef.h)

   纯净原则(见 THEMAP_ARCH.md):
   - 时间线/页面/权限等业务语义一律不进内核, 全部走 attrs 由应用端识别
   - 无 timeline/timestamp 字段(旧内核残留, 不继承)
   - 并发能力由引擎装配(锁旁路表/worker), node 保持纯数据

   本文件是「内核-应用接口」的数据模型部分(接口契约见 API.md)。
   布局约定: 节点定义(node)是核心, 放在构件类型之后立即出现。
   ============================================================ */

#include <stdint.h>
#include <stdbool.h>

/* <stddef.h> 在 MinGW 有 max_align_t 冲突, 用编译器内置类型代替 size_t */
#ifndef __SIZE_TYPE__
#  define __SIZE_TYPE__ unsigned long long
#endif
typedef __SIZE_TYPE__ size_t_;

/* ================= 节点部件(前置微型定义) ================= */

/* ptr_array: 动态指针数组(节点容器, 与大小写无关的通用工具) */
typedef struct {
    void**    items;    // 指针数组(可为 NULL)
    size_t_   count;    // 已用指针数
    size_t_   capacity; // 分配容量(指针数)
} ptr_array;

/* node_attr: 语义键值对 —— 应用端识别, 内核只存不解释。
   约定(见 THEMAP_ARCH.md): role=page / role=axis / grant:<项目> / setting:<k>
   计入 uuid 指纹(仅非空时), 随序列化落盘 */
typedef struct node_attr {
    char* key;
    char* value;
} node_attr;

/* data_kind: 载荷类型 —— 只表达"内容是什么"。
   拓扑角色(是否边节点)由结构判定(node_is_edge: 两端集合都非空),
   载荷与角色正交, 不混入类型枚举 */
typedef enum {
    DATA_TEXT,    /* 文字 */
    DATA_FILE,    /* 文件(路径/URL/二进制) */
} data_kind;

/* ================= 核心: 节点定义 ================= */
/* 前向声明: 让 node 类型在结构体内部可用(边节点自引用需要) */
typedef struct node node;

/* node = 图的基本单元, 既是数据容器也是关系端点。
    - 边也是节点(edge_as/edge_bs 承载两端), 由结构判定非类型
    - 多父/共享(DAG)由 alive 位防二次释放
    - uuid = 除 id/uuid 外全部字段的 SHA256 指纹(内容寻址) */
struct node {
    uint64_t     id;
    data_kind    dtype;      /* 载荷类型(TEXT/FILE) */
    void*        data;       /* 不透明载荷 */
    unsigned     owns_data : 1; /* data 是否由节点自持: 1=destroy 时 free */
    unsigned     alive : 1;     /* 1=活跃; DAG(多父/共享)下防二次释放 */
    size_t_      data_len;   /* data 字节长度(TEXT=strlen) */
    char*        sha256;     /* data 的 SHA256, hex(可为 NULL: 二进制未知) */

    /* 关系(指针集合) */
    ptr_array neighbors;   /* 邻节点(无向) */
    ptr_array childs;      /* 子节点(有向被指向端) */
    ptr_array parents;     /* 父节点(有向指向端) */
    ptr_array edges;       /* 边节点列表 */
    // char*    conn;         /* 邻/子/父节点的uuid列表, 仅供查找 */      // TODO
    /* 边节点专用: 两端都是**集合**, 不是单指针。
       普通关系 = 两端各 1 个; 一对多(收纳/分组) = A 端 1 个; 共享边 = 两端都多个。
       全部形态共用同一个结构, 不设"单边/超边"两种模式。
       角色判定(内容判定式): 两端都非空即边节点 —— 见 node_is_edge。 */
    ptr_array edge_as;     /* 主体端(发出侧) */
    ptr_array edge_bs;     /* 客体端(指向侧) */

    /* 属性表(node_attr*) —— 应用端语义识别 */
    ptr_array attrs;

    char*    uuid;       /* 唯一标识 = 除 id/uuid/conn 外字段的 SHA256 指纹 */
};   /* 定义见上方 typedef struct node node; —— 末尾不再重复 typedef */

// 错误定义 - TODO
#define NullNodeError (-1)
#define MemoryAllocationError (-2)

#endif /* MAPDEF_H */
