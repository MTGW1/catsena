// test-edge.c —— 边节点的"两端集合"模型验证
// ---------------------------------------------------------------------------
// 边 = 两个端集合(A 发出侧 / B 指向侧)都非空的节点。三种形态共用一套结构:
//   普通关系 A→B  : A 1 个, B 1 个
//   一对多   A→…  : A 1 个, B N 个(收纳/分组/一个关系连多个目标)
//   共享边   …→…  : A N 个, B M 个(同一关系被多处共用)
// 本测试钉死这几条性质:
//   1. 1→N 与 N×M 都能建, 且端点计数正确
//   2. 身份与插入顺序无关(同一组端点, 换个顺序加 → uuid 相同)
//   3. 跨存档稳定(存盘→重开, 两端集合与 uuid 都不变)
//   4. 旧档兼容(只有 edge_a_uuid/edge_b_uuid 单值的老存档能读成边)
//   5. 摘掉端点: 某端空了 → 边自动消失; 还有剩 → 边缩水但活着
//   6. 删节点: 边自动收缩, 不留野指针
// 用法: test-edge            (临时文件 test-edge.map, 跑完删除)
// ===========================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "map.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (cond) printf("  ok  %s\n", msg); \
    else { printf("  FAIL %s\n", msg); fails++; } } while (0)

static const char* TMP = "test-edge.map";

static map_node* add_text(map_node* parent, const char* text) {
    char* s = strdup(text);
    if (!s) return NULL;
    map_node* n = map_node_new(MAP_TEXT, s, 0);
    if (!n) { free(s); return NULL; }
    n->owns_data = 1;
    if (parent && map_link_parent_child(parent, n) != 0) { map_node_free(n); return NULL; }
    return n;
}

static map_node* find_by_text(map* m, const char* text) {
    map_node* arr[64];
    size_t n = map_collect_subtree(m, map_root(m), arr, 64);
    for (size_t i = 0; i < n; i++) {
        map_node* x = arr[i];
        if (x->data && x->data_len > 0 && strcmp((const char*)x->data, text) == 0) return x;
    }
    return NULL;
}

/* 数一下图里有几条边(独立于端点计数: 走边上自己), 顺便返回找到的那条。
   边节点不一定在子树里(childs) —— 但 collect_subtree 走的是 childs+邻居+边, 
   所以边节点会随端点被收进来。 */
static int count_edges(map* m, map_node** first) {
    map_node* arr[128];
    size_t n = map_collect_subtree(m, map_root(m), arr, 128);
    int c = 0;
    if (first) *first = NULL;
    for (size_t i = 0; i < n; i++) {
        if (map_node_is_edge(arr[i])) { if (c == 0 && first) *first = arr[i]; c++; }
    }
    return c;
}

// 某节点的 edges 列表里是否含这条边
static int has_edge_ref(map_node* n, map_node* e) {
    for (map_size i = 0; i < n->edges.count; i++) if (n->edges.items[i] == e) return 1;
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== 边节点: 两端集合模型验证 ==\n");
    remove(TMP);

    const char* names[] = { "项目", "甲", "乙", "丙", "丁" };
    const int NV = 5;
    char uuid_1n[65] = {0};
    char uuid_mn[65] = {0};

    // ---- 阶段 1: 建 1→N 和 N×M, 检查端点计数与身份 ----
    {
        MapConfig cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.base.max_workers = 0;
        cfg.enable_index = 1;
        map* m = map_open(TMP, &cfg);
        CHECK(m != NULL && map_root(m) != NULL, "map_open(空图)");
        if (!m) return 1;
        for (int i = 0; i < NV; i++) add_text(map_root(m), names[i]);

        map_node* p = find_by_text(m, "项目");
        map_node* jia = find_by_text(m, "甲");
        map_node* yi  = find_by_text(m, "乙");
        map_node* bing = find_by_text(m, "丙");
        map_node* ding = find_by_text(m, "丁");

        // 1→N: 项目 → {甲, 乙, 丙} 一条边
        map_node* e = map_edge_connect_with(p, jia, strdup("收纳"), 6);
        if (e) e->owns_data = 1;
        CHECK(e != NULL && map_node_is_edge(e), "建 1→1 基础边");
        CHECK(map_edge_end_add(e, yi, 1) == 0, "1→N: 加 B 端第二个");
        CHECK(map_edge_end_add(e, bing, 1) == 0, "1→N: 加 B 端第三个");
        printf("       A=%d B=%d\n", (int)e->edge_as.count, (int)e->edge_bs.count);
        CHECK(e->edge_as.count == 1 && e->edge_bs.count == 3, "1→N: 端点计数 A=1 B=3");
        CHECK(e->edge_as.items[0] == p, "1→N: A 端就是主体");
        CHECK(has_edge_ref(p, e) && has_edge_ref(yi, e) && has_edge_ref(bing, e),
              "1→N: 每个端点都在自己的 edges 列表里挂着这条边");

        // 幂等: 重复加同一个端点不应变成两个
        CHECK(map_edge_end_add(e, yi, 1) == 0 && e->edge_bs.count == 3, "1→N: 重复加端点幂等");

        // N×M 共享边: {甲,乙} → {丙,丁} 一条边
        map_node* s = map_node_new(MAP_TEXT, NULL, 0);
        CHECK(s != NULL, "建共享边节点");
        CHECK(map_edge_end_add(s, jia, 0) == 0 && map_edge_end_add(s, yi, 0) == 0, "共享边: A 端 2 个");
        CHECK(map_edge_end_add(s, bing, 1) == 0 && map_edge_end_add(s, ding, 1) == 0, "共享边: B 端 2 个");
        CHECK(map_node_is_edge(s) && s->edge_as.count == 2 && s->edge_bs.count == 2,
              "共享边: N×M 计数 A=2 B=2");

        // 身份与插入顺序无关: 同样两组端点, 反着加 → 同一个 uuid
        //   (edge_end_add 内部会刷 uuid, 与 link_parent_child 一致)
        map_node* s2 = map_node_new(MAP_TEXT, NULL, 0);
        map_edge_end_add(s2, yi, 0); map_edge_end_add(s2, jia, 0);
        map_edge_end_add(s2, ding, 1); map_edge_end_add(s2, bing, 1);
        CHECK(s->uuid && s2->uuid && strcmp(s->uuid, s2->uuid) == 0,
              "身份与插入顺序无关(同一组端点 = 同一个 uuid)");
        map_node_free(s2);   // 把副本扔掉, 免得存档里多一条

        snprintf(uuid_1n, sizeof(uuid_1n), "%s", e->uuid ? e->uuid : "");
        snprintf(uuid_mn, sizeof(uuid_mn), "%s", s->uuid ? s->uuid : "");
        CHECK(map_save(m, TMP) == 0, "map_save");
        map_close(m);
    }

    // ---- 阶段 2: 重开, 两端集合与 uuid 都应原样回来 ----
    {
        MapConfig cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.base.max_workers = 0;
        cfg.enable_index = 1;
        map* m = map_open(TMP, &cfg);
        CHECK(m != NULL, "map_open(存档)");
        if (!m) return 1;
        map_node* one = NULL;
        int ne = count_edges(m, &one);
        CHECK(ne == 2, "重开后边数 = 2");

        map_node* p = find_by_text(m, "项目");
        map_node* e = NULL;
        for (map_size i = 0; i < p->edges.count; i++) {
            map_node* x = (map_node*)p->edges.items[i];
            if (map_node_is_edge(x) && x->edge_as.count == 1) e = x;
        }
        CHECK(e != NULL && e->edge_bs.count == 3, "1→N 跨存档: A=1 B=3");
        if (e) {
            CHECK(e->uuid && strcmp(e->uuid, uuid_1n) == 0, "1→N 跨存档: uuid 不变");
            CHECK(e->data && strcmp((const char*)e->data, "收纳") == 0, "1→N 跨存档: 边名不变");
            int all = 1;
            for (map_size i = 0; i < e->edge_bs.count; i++) {
                map_node* b = (map_node*)e->edge_bs.items[i];
                if (!has_edge_ref(b, e)) all = 0;         // 反向引用也要恢复
            }
            CHECK(all, "1→N 跨存档: 每个 B 端的 edges 里都有它");
        }

        // 共享边
        map_node* s = NULL;
        map_node* arr[64];
        size_t n = map_collect_subtree(m, map_root(m), arr, 64);
        for (size_t i = 0; i < n; i++)
            if (map_node_is_edge(arr[i]) && arr[i]->edge_as.count == 2) s = arr[i];
        CHECK(s != NULL && s->edge_bs.count == 2, "共享边跨存档: A=2 B=2");
        CHECK(s && s->uuid && strcmp(s->uuid, uuid_mn) == 0, "共享边跨存档: uuid 不变");

        // ---- 阶段 3: 摘端点 ----
        map_node* ding = find_by_text(m, "丁");
        if (s) {
            CHECK(map_edge_end_del(s, ding, 1) == 0, "共享边: 摘掉一个 B 端");
            CHECK(map_node_is_edge(s) && s->edge_bs.count == 1, "摘一个端点后边还活着(缩水)");
            CHECK(!has_edge_ref(ding, s), "被摘掉的端点不再挂这条边");
            // 把 A 端摘空 → 关系不成立
            map_node* jia = find_by_text(m, "甲");
            map_node* yi = find_by_text(m, "乙");
            map_edge_end_del(s, jia, 0); map_edge_end_del(s, yi, 0);
            CHECK(!map_node_is_edge(s), "A 端摘空 → 不再是边");
            map_node* keep_a = find_by_text(m, "丙");
            (void)keep_a;
            map_node_free(s);   // 引擎在命令层会自动删, 这里手动收尾
        }

        // ---- 阶段 4: 删节点 → 边自动收缩 ----
        map_node* e2 = NULL;
        for (map_size i = 0; i < p->edges.count; i++) {
            map_node* x = (map_node*)p->edges.items[i];
            if (map_node_is_edge(x)) e2 = x;
        }
        if (e2) {
            map_node* bing = NULL;
            for (map_size i = 0; i < e2->edge_bs.count; i++) {
                map_node* b = (map_node*)e2->edge_bs.items[i];
                if (b->data && strcmp((const char*)b->data, "丙") == 0) bing = b;
            }
            CHECK(bing != NULL, "找得到 B 端的丙");
            if (bing) {
                map_size before = e2->edge_bs.count;
                map_node_free(bing);   // 删丙
                CHECK(e2->edge_bs.count == before - 1, "删端点节点 → 边的 B 端自动少一个");
                CHECK(map_node_is_edge(e2), "B 端还有剩 → 边仍在");
            }
        }
        map_close(m);
    }

    // ---- 阶段 5: 旧档兼容(只写单值的 edge_a_uuid/edge_b_uuid) ----
    {
        const char* legacy = "test-edge-legacy.map";
        remove(legacy);
        // 手写一份"旧格式"存档: 两个文字节点 + 一条只有单值端点的边
        FILE* f = fopen(legacy, "wb");
        if (f) {
            fputs("{\"data_hex\":\"6f6c64\",\"id\":1,\"children\":["
                  "{\"data_hex\":\"41\",\"id\":2,\"uuid\":\"aaaa\",\"parents_uuid\":[\"6f6c64\"]},"
                  "{\"data_hex\":\"42\",\"id\":3,\"uuid\":\"bbbb\",\"parents_uuid\":[\"6f6c64\"]}"
                  "],\"edges\":[{\"id\":4,\"uuid\":\"cccc\",\"edge_a_uuid\":\"aaaa\",\"edge_b_uuid\":\"bbbb\"}]}",
                  f);
            fclose(f);
        }
        MapConfig cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.base.max_workers = 0;
        map* m = map_open(legacy, &cfg);
        CHECK(m != NULL, "map_open(旧格式存档)");
        if (m) {
            map_node* e = NULL;
            int ne = count_edges(m, &e);
            CHECK(ne == 1, "旧档: 读成 1 条边");
            CHECK(e != NULL && e->edge_as.count == 1 && e->edge_bs.count == 1,
                  "旧档: 两端各 1 个");
            if (e) printf("       旧档边 uuid=%.12s\n", e->uuid ? e->uuid : "(null)");
            map_close(m);
        }
        remove(legacy);
    }

    remove(TMP);
    printf("== 结果: %s (%d 失败) ==\n", fails == 0 ? "PASS" : "FAIL", fails);
    return fails == 0 ? 0 : 1;
}
