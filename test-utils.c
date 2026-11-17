// test-utils.c —— utils.h 三个辅助函数的验证
// ---------------------------------------------------------------------------
// 这三个函数是"图上的常用问答"，公开之前必须先钉死语义：
//   1. map_select_exposed_nodes  —— 框选边界：哪些节点连到选区外，外面是谁
//   2. map_nodes_may_know        —— 两点是否同一子图（无向连通）
//   3. map_node_related_uuids    —— 周围有谁（位掩码组合 + 去重）
// 另外验证内存约定：所有 char** 输出必须能用 map_uuids_free() 一次释放干净。
//
// 造图（A/B 各有子节点，跨枝一条邻接 + 一条边节点）：
//     root ─ A ─ A1
//          │   └ A2 ┄┄ B1
//          │        ┆
//          │      (E 边)
//          └ B ─ B1
// ===========================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "map.h"
#include "utils.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (cond) printf("  ok  %s\n", msg); \
    else { printf("  FAIL %s\n", msg); fails++; } } while (0)

static map_node* add_text(map_node* parent, const char* text) {
    char* s = strdup(text);
    if (!s) return NULL;
    map_node* n = map_node_new(MAP_TEXT, s, 0);
    if (!n) { free(s); return NULL; }
    n->owns_data = 1;
    if (parent && map_link_parent_child(parent, n) != 0) { map_node_free(n); return NULL; }
    return n;
}

static int has_uuid(char** arr, size_t n, const char* uuid) {
    if (!arr || !uuid) return 0;
    for (size_t i = 0; i < n; i++) {
        if (arr[i] && strcmp(arr[i], uuid) == 0) return 1;
    }
    return 0;
}

static int all_unique(char** arr, size_t n) {
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (arr[i] && arr[j] && strcmp(arr[i], arr[j]) == 0) return 0;
        }
    }
    return 1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== utils 辅助函数验证 ==\n");

    MapConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    map* m = map_open(NULL, &cfg);
    CHECK(m != NULL, "map_open(空图)");
    if (!m) { printf("\n== 结果: FAIL ==\n"); return 1; }

    map_node* root = map_root(m);
    map_node* A  = add_text(root, "A");
    map_node* B  = add_text(root, "B");
    map_node* A1 = add_text(A, "A1");
    map_node* A2 = add_text(A, "A2");
    map_node* B1 = add_text(B, "B1");
    map_node* C  = add_text(root, "C");           /* 孤立节点 */
    CHECK(A && B && A1 && A2 && B1 && C, "建节点 root:A(A1,A2) B(B1) C");

    CHECK(map_link_neighbor(A2, B1) == 0, "跨枝邻接 A2 ┄ B1");
    map_node* E = map_edge_connect(A, B);
    CHECK(E != NULL, "边节点 E: A ─ B");

    /* ---- 3. related: 子/父/全 + 位掩码校验 + 去重 ---- */
    char** uu = NULL;
    size_t un = 0;

    CHECK(map_node_related_uuids(A, MAP_REL_CHILD, &uu, &un) == 0 && un == 2,
          "related(A, CHILD) = 2 个子");
    CHECK(has_uuid(uu, un, A1->uuid) && has_uuid(uu, un, A2->uuid), "  子节点 uuid 正确");
    map_uuids_free(uu, un);

    uu = NULL; un = 0;
    CHECK(map_node_related_uuids(A, MAP_REL_PARENT, &uu, &un) == 0 && un == 1 &&
          has_uuid(uu, un, root->uuid), "related(A, PARENT) = root");
    map_uuids_free(uu, un);

    uu = NULL; un = 0;
    int rc_all = map_node_related_uuids(A, MAP_REL_ALL, &uu, &un);
    CHECK(rc_all == 0 && un >= 3, "related(A, ALL) ⊇ 2 子 + 父");
    CHECK(has_uuid(uu, un, A1->uuid) && has_uuid(uu, un, A2->uuid) && has_uuid(uu, un, root->uuid),
          "  ALL 覆盖子与父");
    CHECK(all_unique(uu, un), "  ALL 结果无重复(去重生效)");
    map_uuids_free(uu, un);

    uu = (char**)1; un = 99;
    CHECK(map_node_related_uuids(A, 0, &uu, &un) == -1 && uu == NULL && un == 0,
          "related(mode=0) 拒绝并清空输出");
    uu = (char**)1; un = 99;
    CHECK(map_node_related_uuids(A, 8, &uu, &un) == -1, "related(mode=8) 越界拒绝");
    CHECK(map_node_related_uuids(NULL, MAP_REL_ALL, &uu, &un) == -1, "related(NULL 节点) 拒绝");

    /* ---- 2. may_know: 连通 / 不连通 / 自反 ---- */
    CHECK(map_nodes_may_know(m, A2, B1) == 1, "may_know(A2,B1) 跨枝邻接连通");
    CHECK(map_nodes_may_know(m, A1, root) == 1, "may_know(A1,root) 父子连通");
    CHECK(map_nodes_may_know(m, A, A) == 1, "may_know(A,A) 自反");
    /* 注意: C 与 A1 都挂在 root 下, 经 root 是连通的 —— 树状结构里"可能认识"
       对任何两个节点都成立。要测不连通必须造独立分量: D 从未被链接过。 */
    map_node* D = add_text(NULL, "D");            /* 独立分量 */
    CHECK(D != NULL, "建独立节点 D(未链接)");
    CHECK(map_nodes_may_know(m, C, A1) == 1, "may_know(C,A1) 经 root 连通(树状结构)");
    CHECK(map_nodes_may_know(m, D, A1) == 0, "may_know(D,A1) 独立分量不连通");
    CHECK(map_nodes_may_know(m, NULL, A1) == -1, "may_know(NULL) 拒绝");

    /* ---- 1. select_exposed: 选区 / 边界 / 外部 ---- */
    char** ex = NULL; size_t exn = 0;
    char** out = NULL; size_t outn = 0;

    const map_node* sel1[3] = { A, A1, A2 };
    CHECK(map_select_exposed_nodes(m, sel1, 3, &ex, &exn, &out, &outn) == 0,
          "select_exposed(选区={A,A1,A2})");
    CHECK(exn == 2, "  边界节点 2 个(A 连着 root/E, A2 连着 B1)");
    CHECK(has_uuid(ex, exn, A->uuid) && has_uuid(ex, exn, A2->uuid),
          "  边界 = {A, A2}");
    CHECK(!has_uuid(ex, exn, A1->uuid), "  内部节点 A1 不在边界里");
    CHECK(has_uuid(out, outn, root->uuid), "  外部含 root");
    CHECK(has_uuid(out, outn, B1->uuid), "  外部含 B1（A2 的跨枝邻居）");
    CHECK(all_unique(out, outn), "  外部列表去重");
    /* 一个节点连出去多条边时, 外部集合不能只收到一条(旧实现只收第一条) */
    CHECK(outn >= 3, "  外部 ≥3 条(不是每个边界节点只收一条)");
    map_uuids_free(ex, exn);
    map_uuids_free(out, outn);

    /* 全选 = 没有外部（除 root 之外全在里面 → root 仍在外面） */
    ex = NULL; exn = 0; out = NULL; outn = 0;
    /* 边节点 E 也必须选进去: 它不在选区时, A/B 的 edges 就指向选区外, 外部集合多一个 E */
    const map_node* sel2[7] = { A, B, A1, A2, B1, C, E };
    CHECK(map_select_exposed_nodes(m, sel2, 7, &ex, &exn, &out, &outn) == 0,
          "select_exposed(全选, 含边节点 E)");
    /* C 也是 root 的孩子而 root 不在选区 → C 同样是边界节点 */
    CHECK(exn == 3 && has_uuid(ex, exn, A->uuid) && has_uuid(ex, exn, B->uuid) &&
          has_uuid(ex, exn, C->uuid),
          "  全选时边界={A,B,C}(都连着选区外的 root)");
    CHECK(outn == 1 && has_uuid(out, outn, root->uuid), "  外部只有 root");
    map_uuids_free(ex, exn);
    map_uuids_free(out, outn);

    /* 参数校验: 失败时输出必须清空, 释放安全 */
    ex = (char**)1; exn = 99; out = (char**)1; outn = 99;
    CHECK(map_select_exposed_nodes(m, sel1, 0, &ex, &exn, &out, &outn) == -1 &&
          ex == NULL && out == NULL && exn == 0 && outn == 0,
          "select_exposed(空选区) 拒绝并清空输出");
    CHECK(map_select_exposed_nodes(NULL, sel1, 3, &ex, &exn, &out, &outn) == -1,
          "select_exposed(m=NULL) 拒绝");
    map_uuids_free(NULL, 0);   /* 必须安全 */
    CHECK(1, "map_uuids_free(NULL,0) 安全");

    map_close(m);
    printf("\n== 结果: %s (%d 失败) ==\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
