// utils.c —— catsena 图查询辅助函数集（零外部依赖，零警告）
//
// 这三个函数是"图上的常用问答"：
//   1. 框选了一堆节点 —— 哪些是边界节点，外面连着谁？
//   2. 两个节点之间有路吗（同一子图）？
//   3. 一个节点周围都有谁？
//
// 内存约定：所有 char** 输出都由本文件分配，调用方必须用 map_uuids_free() 释放
// （不是 free(arr)，那是泄一半）。
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// 节点的一级关系数组一共 6 个：邻/父/子/边，以及边节点专用的 A 端/B 端集合。
#define REL_COUNT 6

static map_ptr_array* rel_at(const map_node* n, int idx) {
    switch (idx) {
        case 0: return (map_ptr_array*)&n->neighbors;
        case 1: return (map_ptr_array*)&n->parents;
        case 2: return (map_ptr_array*)&n->childs;
        case 3: return (map_ptr_array*)&n->edges;
        case 4: return (map_ptr_array*)&n->edge_as;   /* 边节点专用: 主体端集合 */
        case 5: return (map_ptr_array*)&n->edge_bs;   /* 边节点专用: 客体端集合 */
        default: return NULL;
    }
}

// ---------- 内部辅助：动态字符串数组追加 ----------
static int strarr_push(char*** arr, size_t* count, size_t* cap, const char* str) {
    if (!arr || !count || !str) return -1;
    if (*count >= *cap) {
        size_t newcap = (*cap == 0) ? 8 : (*cap * 2);
        char** newarr = (char**)realloc((void*)*arr, newcap * sizeof(char*));
        if (!newarr) return -1;
        *arr = newarr;
        *cap = newcap;
    }
    size_t len = strlen(str);
    char* dup = (char*)malloc(len + 1);
    if (!dup) return -1;
    memcpy(dup, str, len + 1);
    (*arr)[(*count)++] = dup;
    return 0;
}

// ---------- 内部辅助：节点指针集合（线性探测 + 去重） ----------
typedef struct {
    map_node** items;
    size_t count;
    size_t cap;
} node_set;

static int node_set_add(node_set* s, map_node* n) {
    if (!s || !n) return -1;
    for (size_t i = 0; i < s->count; i++) {
        if (s->items[i] == n) return 0;
    }
    if (s->count >= s->cap) {
        size_t newcap = (s->cap == 0) ? 64 : s->cap * 2;
        map_node** tmp = (map_node**)realloc((void*)s->items, newcap * sizeof(map_node*));
        if (!tmp) return -1;
        s->items = tmp;
        s->cap = newcap;
    }
    s->items[s->count++] = n;
    return 0;
}

static int node_set_contains(const node_set* s, const map_node* n) {
    for (size_t i = 0; i < s->count; i++) {
        if (s->items[i] == n) return 1;
    }
    return 0;
}

static void node_set_free(node_set* s) {
    if (s) {
        free((void*)s->items);
        s->items = NULL;
        s->count = 0;
        s->cap = 0;
    }
}

// ============================================================================
// 0. 释放 uuid 数组（所有返回 char** 的函数共用这一个出口）
// ============================================================================
void map_uuids_free(char** uuids, size_t count) {
    if (!uuids) return;
    for (size_t i = 0; i < count; i++) {
        free((void*)uuids[i]);
    }
    free((void*)uuids);
}

// ============================================================================
// 1. 框选分析：选出"边界节点"（有关系到选区外）并汇总所有外部邻居
// ============================================================================
int map_select_exposed_nodes(
    map*         m,
    const map_node** selected_nodes,
    size_t       selected_count,
    char***      out_uuids,
    size_t*      out_count,
    char***      out_outside_uuids,
    size_t*      out_outside_count
) {
    // 先把所有输出置成"空", 这样任何失败路径下调用方都能安全释放。
    if (out_uuids)          *out_uuids = NULL;
    if (out_count)          *out_count = 0;
    if (out_outside_uuids)  *out_outside_uuids = NULL;
    if (out_outside_count)  *out_outside_count = 0;

    if (!m || !selected_nodes || selected_count == 0 ||
        !out_uuids || !out_count || !out_outside_uuids || !out_outside_count) {
        return -1;
    }
    (void)m;   /* 纯查询, 不取锁也不改图; 保留入参为了和另外两个函数签名一致 */

    int ret = -1;
    node_set inside = {0};
    node_set outside_set = {0};
    char** exposed = NULL;
    size_t exposed_count = 0, exposed_cap = 0;
    char** outside = NULL;

    // 选区（去重）
    for (size_t i = 0; i < selected_count; i++) {
        map_node* n = (map_node*)selected_nodes[i];
        if (n && node_set_add(&inside, n) != 0) goto cleanup;
    }

    // 逐个检查选区内的节点：任意一条关系落在选区外 → 它是边界节点
    for (size_t i = 0; i < inside.count; i++) {
        map_node* cur = inside.items[i];
        int exposed_here = 0;

        for (int r = 0; r < REL_COUNT; r++) {
            map_ptr_array* rels = rel_at(cur, r);
            if (!rels) continue;
            for (size_t j = 0; j < rels->count; j++) {
                map_node* rel = (map_node*)rels->items[j];
                if (!rel) continue;
                if (node_set_contains(&inside, rel)) continue;
                // 落在外面: 记标记 + 收进外部集合（继续扫, 一个节点可能连出去好几条）
                exposed_here = 1;
                if (node_set_add(&outside_set, rel) != 0) goto cleanup;
            }
        }

        if (exposed_here && cur->uuid) {
            if (strarr_push(&exposed, &exposed_count, &exposed_cap, cur->uuid) != 0) goto cleanup;
        }
    }

    // 外部节点 → uuid 数组（cap 正好等于条目数, 扩容不会发生）
    size_t outside_n = 0, outside_cap = outside_set.count;
    if (outside_set.count > 0) {
        outside = (char**)calloc(outside_set.count, sizeof(char*));
        if (!outside) goto cleanup;
        for (size_t i = 0; i < outside_set.count; i++) {
            const char* u = outside_set.items[i]->uuid;
            if (strarr_push(&outside, &outside_n, &outside_cap, u ? u : "") != 0) goto cleanup;
        }
    }

    // 全部成功才交付所有权
    *out_uuids = exposed;
    *out_count = exposed_count;
    *out_outside_uuids = outside;
    *out_outside_count = outside_n;
    exposed = NULL;
    outside = NULL;
    ret = 0;

cleanup:
    map_uuids_free(exposed, exposed_count);
    map_uuids_free(outside, outside_n);
    node_set_free(&inside);
    node_set_free(&outside_set);
    return ret;
}

// ============================================================================
// 2. 两个节点是否在同一张子图（无向连通）
// ============================================================================
int map_nodes_may_know(map* m, const map_node* node_a, const map_node* node_b) {
    if (!m || !node_a || !node_b) return -1;
    if (node_a == node_b) return 1;
    (void)m;

    node_set visited = {0};
    map_node** stack = NULL;
    size_t stack_cap = 0, stack_count = 0;
    int ret = 0;

    #define PUSH(n) do { \
        if (!(n)) break; \
        if (node_set_contains(&visited, (n))) break; \
        if (node_set_add(&visited, (map_node*)(n)) != 0) goto cleanup; \
        if (stack_count >= stack_cap) { \
            size_t nc = (stack_cap == 0) ? 64 : stack_cap * 2; \
            map_node** tmp = (map_node**)realloc((void*)stack, nc * sizeof(map_node*)); \
            if (!tmp) goto cleanup; \
            stack = tmp; \
            stack_cap = nc; \
        } \
        stack[stack_count++] = (map_node*)(n); \
    } while(0)

    PUSH(node_a);

    while (stack_count > 0) {
        map_node* cur = stack[--stack_count];
        if (cur == node_b) { ret = 1; goto cleanup; }
        for (int r = 0; r < REL_COUNT; r++) {
            map_ptr_array* rels = rel_at(cur, r);
            if (!rels) continue;
            for (size_t i = 0; i < rels->count; i++) {
                map_node* rel = (map_node*)rels->items[i];
                if (rel) PUSH(rel);
            }
        }
    }

cleanup:
    #undef PUSH
    free((void*)stack);
    node_set_free(&visited);
    return ret;
}

// ============================================================================
// 3. 节点周围的 uuid（邻居/子/父，位掩码组合，结果去重）
// ============================================================================
int map_node_related_uuids(const map_node* n, int mode, char*** out_uuids, size_t* out_count) {
    if (out_uuids) *out_uuids = NULL;
    if (out_count) *out_count = 0;
    if (!n || !out_uuids || !out_count) return -1;

    // mode 是位掩码: 1=邻居 2=子节点 4=父节点(7=全部)。0 或越界视为错误,
    // 早期文档写成 0/1/2/3 的枚举(和实现对不上), 这里以位掩码为准。
    if (mode <= 0 || mode >= 8) return -1;

    char** result = NULL;
    size_t count = 0, cap = 0;
    node_set seen = {0};
    int ret = -1;

    #define ADD_REL(idx) do { \
        map_ptr_array* rels = rel_at(n, (idx)); \
        if (rels) { \
            for (size_t i = 0; i < rels->count; i++) { \
                map_node* rel = (map_node*)rels->items[i]; \
                if (!rel || !rel->uuid) continue; \
                size_t before = seen.count; \
                if (node_set_add(&seen, rel) != 0) goto cleanup; \
                if (seen.count == before) continue;            /* 已收过, 去重 */ \
                if (strarr_push(&result, &count, &cap, rel->uuid) != 0) goto cleanup; \
            } \
        } \
    } while(0)

    if (mode & 1) ADD_REL(0);
    if (mode & 4) ADD_REL(1);
    if (mode & 2) ADD_REL(2);

    *out_uuids = result;
    *out_count = count;
    result = NULL;
    ret = 0;

cleanup:
    #undef ADD_REL
    map_uuids_free(result, count);
    node_set_free(&seen);
    return ret;
}
