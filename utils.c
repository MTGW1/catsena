// utils.c —— catsena 实用工具函数集（零外部依赖，零警告）
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>



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
    char* dup = strdup(str);
    if (!dup) return -1;
    (*arr)[(*count)++] = dup;
    return 0;
}

// ---------- 内部辅助：节点指针集合（线性探测） ----------
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
// 1. 找选定节点中“有外部连接”的节点，以及所有外部邻居的 UUID（去重）
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
    if (!m || !selected_nodes || selected_count == 0 ||
        !out_uuids || !out_count || !out_outside_uuids || !out_outside_count) {
        return -1;
    }

    int ret = -1;
    node_set inside = {0};
    node_set outside_set = {0};
    char** result_uuids = NULL;
    size_t result_cap = 0, result_count = 0;

    // 构建内部集合
    for (size_t i = 0; i < selected_count; i++) {
        map_node* n = (map_node*)selected_nodes[i];
        if (n) {
            if (node_set_add(&inside, n) != 0) goto cleanup;
        }
    }

    // 遍历内部节点检查关系
    for (size_t i = 0; i < inside.count; i++) {
        map_node* cur = inside.items[i];
        int has_outside = 0;

        map_ptr_array* rels[4] = { &cur->neighbors, &cur->parents, &cur->childs, &cur->edges };
        for (int r = 0; r < 4 && !has_outside; r++) {
            for (size_t j = 0; j < rels[r]->count; j++) {
                map_node* rel = (map_node*)rels[r]->items[j];
                if (!rel) continue;
                if (!node_set_contains(&inside, rel)) {
                    has_outside = 1;
                    if (node_set_add(&outside_set, rel) != 0) goto cleanup;
                    break;
                }
            }
        }

        if (has_outside) {
            if (strarr_push(&result_uuids, &result_count, &result_cap, cur->uuid) != 0) {
                goto cleanup;
            }
        }
    }

    // 输出内部边界节点
    *out_count = result_count;
    *out_uuids = result_uuids;
    result_uuids = NULL; // 所有权转移

    // 输出外部节点
    *out_outside_count = outside_set.count;
    if (outside_set.count > 0) {
        char** out_out = (char**)malloc(outside_set.count * sizeof(char*));
        if (!out_out) goto cleanup;
        for (size_t i = 0; i < outside_set.count; i++) {
            out_out[i] = strdup(outside_set.items[i]->uuid ? outside_set.items[i]->uuid : "");
            if (!out_out[i]) {
                for (size_t j = 0; j < i; j++) free((void*)out_out[j]);
                free((void*)out_out);
                goto cleanup;
            }
        }
        *out_outside_uuids = out_out;
    } else {
        *out_outside_uuids = NULL;
    }

    ret = 0;

cleanup:
    if (result_uuids) {
        for (size_t i = 0; i < result_count; i++) free((void*)result_uuids[i]);
        free((void*)result_uuids);
    }
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
        map_ptr_array* rels[4] = { &cur->neighbors, &cur->parents, &cur->childs, &cur->edges };
        for (int r = 0; r < 4; r++) {
            for (size_t i = 0; i < rels[r]->count; i++) {
                map_node* rel = (map_node*)rels[r]->items[i];
                if (rel) PUSH(rel);
            }
        }
        // 边节点: 两端都是集合, 全部推进栈(1→N / 共享边都算连通)
        for (size_t i = 0; i < cur->edge_as.count; i++) {
            map_node* e = (map_node*)cur->edge_as.items[i];
            if (e) PUSH(e);
        }
        for (size_t i = 0; i < cur->edge_bs.count; i++) {
            map_node* e = (map_node*)cur->edge_bs.items[i];
            if (e) PUSH(e);
        }
    }

cleanup:
    free((void*)stack);
    node_set_free(&visited);
    return ret;
}



// ============================================================================
// 4. 获取节点相关 UUID（邻居/子/父，组合）
// ============================================================================
int map_node_related_uuids(const map_node* n, int mode, char*** out_uuids, size_t* out_count) {
    if (!n || !out_uuids || !out_count) return -1;
    int include_neighbors = (mode & 1) != 0;
    int include_childs    = (mode & 2) != 0;
    int include_parents   = (mode & 4) != 0;

    char** result = NULL;
    size_t count = 0, cap = 0;
    int ret = -1;

    #define ADD_REL(rel_array) do { \
        for (size_t i = 0; i < (rel_array)->count; i++) { \
            map_node* rel = (map_node*)(rel_array)->items[i]; \
            if (rel && rel->uuid) { \
                if (strarr_push(&result, &count, &cap, rel->uuid) != 0) goto cleanup; \
            } \
        } \
    } while(0)

    if (include_neighbors) ADD_REL(&n->neighbors);
    if (include_childs)    ADD_REL(&n->childs);
    if (include_parents)   ADD_REL(&n->parents);

    *out_uuids = result;
    *out_count = count;
    ret = 0;

cleanup:
    if (ret != 0) {
        for (size_t i = 0; i < count; i++) free((void*)result[i]);
        free((void*)result);
    }
    return ret;
}