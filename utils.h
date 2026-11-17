#ifndef CATSENA_UTILS_H
#define CATSENA_UTILS_H

/* ============================================================
   catsena/utils.h —— 图查询辅助函数（可选的高级问答）

   内核本体（map.h）只给"图操作"的原子件；这里给的是建在上面的常用问答：
     1. 框选了一堆节点 —— 哪些是边界节点，外面连着谁？
     2. 两个节点之间有路吗（同一张子图）？
     3. 一个节点周围都有谁？

   头文件不依赖 map.c 的内部结构，只用 mapdef.h 里的公开字段（neighbors/parents/
   childs/edges/edge_as/edge_bs），所以可以放心装进 include/。

   内存约定（三处一致）：
     所有 char** 输出都由本模块分配，调用方一律用 map_uuids_free() 释放 ——
     不要自己 free(uuids), 那样只释放数组、泄掉每个字符串。
   ============================================================ */

#include <stddef.h>
#include "mapdef.h"
#include "map.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 释放 uuid 数组（所有返回 char** 的函数的唯一正确出口） ----
   传 NULL 安全；count 必须是函数返回的那个个数。 */
void map_uuids_free(char** uuids, size_t count);

/* ---- 1. 框选分析 ----
   选区内的节点若有一条关系指向选区外，它就是"边界节点"。

   输入:
     m              图实例（本函数是纯查询：不取锁、不改图；传 NULL 返回 -1）
     selected_nodes 选定节点数组（可含重复/含 NULL，内部会去重跳过）
     selected_count 选定数量
   输出:
     out_uuids          边界节点的 uuid 数组（用 map_uuids_free 释放）
     out_count          边界节点个数
     out_outside_uuids  所有外部邻居的 uuid 数组（去重）
     out_outside_count  外部邻居个数
   返回: 0 成功, -1 失败（失败时四个输出都被置成 NULL/0, 释放是安全的） */
int map_select_exposed_nodes(
    map*             m,
    const map_node** selected_nodes,
    size_t           selected_count,
    char***          out_uuids,
    size_t*          out_count,
    char***          out_outside_uuids,
    size_t*          out_outside_count
);

/* ---- 2. 两个节点是否在同一张子图（沿所有关系做无向连通判定） ----
   关系包含 邻/父/子/边，以及边节点两端集合（1→N、N×M 共享边都算连通）。
   返回: 1 连通（"可能认识"）, 0 不连通, -1 参数错误 */
int map_nodes_may_know(map* m, const map_node* node_a, const map_node* node_b);

/* ---- 3. 节点周围的 uuid（去重） ----
   mode 是**位掩码**（注意: 不是 0/1/2 的枚举）:
     MAP_REL_NEIGHBOR(1) | MAP_REL_CHILD(2) | MAP_REL_PARENT(4)
     7 = 全部三种
   返回: 0 成功, -1 参数错误（mode 为 0 或越界也返回 -1）
   输出: out_uuids/out_count, 用 map_uuids_free 释放 */

#define MAP_REL_NEIGHBOR 1
#define MAP_REL_CHILD    2
#define MAP_REL_PARENT   4
#define MAP_REL_ALL      (MAP_REL_NEIGHBOR | MAP_REL_CHILD | MAP_REL_PARENT)

int map_node_related_uuids(const map_node* n, int mode, char*** out_uuids, size_t* out_count);

#ifdef __cplusplus
}
#endif

#endif /* CATSENA_UTILS_H */
