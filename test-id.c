// test-id.c —— id(持久句柄) 与 uuid(内容指纹) 的分工验证
// ---------------------------------------------------------------------------
// 视图态(坐标/显示态/选中)以 id 为 key, 语义引用以 uuid 为 key(DATA_MODEL.md §1)。
// 这依赖两条性质, 本测试逐条钉死:
//   1. id 跨存档稳定   —— 存盘→重开, 每个节点的 id 不变
//   2. id 不随编辑变   —— 改内容只换 uuid, id 不动(否则改一个字就重置视图态)
//   3. id 不撞车       —— 还原存档后新建节点, 其 id 必须大于所有已还原 id
//   4. uuid 仍随内容变 —— 内容寻址语义不被破坏
// 用法: test-id            (临时文件 test-id.map, 跑完删除)
// ===========================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "map.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (cond) printf("  ok  %s\n", msg); \
    else { printf("  FAIL %s\n", msg); fails++; } } while (0)

static const char* TMP = "test-id.map";

// 建一个文字节点挂到 parent 下(data 用 strdup 并交给节点自持)
static map_node* add_text(map_node* parent, const char* text) {
    char* s = strdup(text);
    if (!s) return NULL;
    map_node* n = map_node_new(MAP_TEXT, s, 0);
    if (!n) { free(s); return NULL; }
    n->owns_data = 1;
    if (parent && map_link_parent_child(parent, n) != 0) { map_node_free(n); return NULL; }
    return n;
}

// 按 data(标题)在子树里找节点 —— 不用 uuid, 避免受 uuid 重算影响
static map_node* find_by_text(map* m, const char* text) {
    map_node* arr[64];
    size_t n = map_collect_subtree(m, map_root(m), arr, 64);
    for (size_t i = 0; i < n; i++) {
        map_node* x = arr[i];
        if (x->data && x->data_len > 0 && strcmp((const char*)x->data, text) == 0) return x;
    }
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== id / uuid 分工验证 ==\n");
    remove(TMP);

    const char* names[] = { "立项", "评审", "开发", "上线", "复盘" };
    const int   NV = 5;
    uint64_t id0[NV];
    char     uuid0[NV][65];

    // ---- 阶段 1: 建图并记录 id / uuid ----
    {
        MapConfig cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.base.max_workers = 0;
        cfg.enable_index = 1;
        map* m = map_open(TMP, &cfg);          // 文件不存在 → 空图
        CHECK(m != NULL && map_root(m) != NULL, "map_open(空图)");
        if (!m) return 1;
        map_node* root = map_root(m);
        for (int i = 0; i < NV; i++) {
            map_node* n = add_text(root, names[i]);
            if (!n) { printf("  FAIL 建节点 %s\n", names[i]); return 1; }
        }
        map_node* a = find_by_text(m, "立项");
        map_node* b = find_by_text(m, "评审");
        map_node* e = map_edge_connect_with(a, b, strdup("先后"), 6);
        CHECK(e != NULL && map_node_is_edge(e), "建边节点");
        map_attr_set(e, "view", "range");      // 边也可以带渲染标签(载荷与角色正交)

        // 基线必须在"图结构稳定之后"取: 连边会改端点的边计数 → 端点 uuid 会变
        //   (这正是 uuid 不能做视图态 key 的理由 —— id 不受影响)
        for (int i = 0; i < NV; i++) {
            map_node* n = find_by_text(m, names[i]);
            id0[i] = n->id;
            snprintf(uuid0[i], sizeof(uuid0[i]), "%s", n->uuid ? n->uuid : "");
        }

        // id 唯一性(进程内)
        int uniq = 1;
        for (int i = 0; i < NV; i++)
            for (int j = i + 1; j < NV; j++)
                if (id0[i] == id0[j]) uniq = 0;
        CHECK(uniq, "新建节点 id 互不相同");
        printf("       [基线] root=%.12s childs=%d\n",
               map_root(m)->uuid ? map_root(m)->uuid : "(null)", (int)map_root(m)->childs.count);

        CHECK(map_save(m, TMP) == 0, "map_save");
        map_close(m);
    }

    // ---- 阶段 2: 重开, 逐条比对 id(性质 1) ----
    {
        MapConfig cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.base.max_workers = 0;
        cfg.enable_index = 1;
        map* m = map_open(TMP, &cfg);
        CHECK(m != NULL && map_root(m) != NULL, "map_open(存档)");
        if (!m) return 1;
        int same = 1, same_uuid = 1;
        uint64_t maxid = 0;
        printf("       [重开] root=%.12s childs=%d\n",
               map_root(m)->uuid ? map_root(m)->uuid : "(null)", (int)map_root(m)->childs.count);
        for (int i = 0; i < NV; i++) {
            map_node* n = find_by_text(m, names[i]);
            if (!n) { printf("  FAIL 重开后找不到 %s\n", names[i]); fails++; same = 0; continue; }
            if (n->id != id0[i]) {
                printf("       %s: id %llu → %llu\n", names[i],
                       (unsigned long long)id0[i], (unsigned long long)n->id);
                same = 0;
            }
            if (!n->uuid || strcmp(n->uuid, uuid0[i]) != 0) {
                printf("       [uuid 变] %s: %.12s → %.12s (attrs=%d edges=%d)\n", names[i],
                       uuid0[i], n->uuid ? n->uuid : "(null)",
                       (int)n->attrs.count, (int)n->edges.count);
                same_uuid = 0;
            }
            if (n->id > maxid) maxid = n->id;
        }
        CHECK(same, "性质1: id 跨存档稳定");
        CHECK(same_uuid, "性质4: uuid 跨存档稳定(内容寻址不受影响)");

        // ---- 阶段 3: 编辑内容 → uuid 变, id 不变(性质 2) ----
        map_node* n = find_by_text(m, "评审");
        char old_uuid[65];
        snprintf(old_uuid, sizeof(old_uuid), "%s", n->uuid ? n->uuid : "");
        uint64_t old_id = n->id;
        map_attr_set(n, "at", "96");            // 语义编辑(进指纹)
        CHECK(n->id == old_id, "性质2: 编辑后 id 不变");
        CHECK(n->uuid && strcmp(n->uuid, old_uuid) != 0, "性质4: 编辑后 uuid 改变");

        // ---- 阶段 4: 新建节点 id 不撞车(性质 3) ----
        map_node* fresh = add_text(map_root(m), "新增");
        CHECK(fresh != NULL && fresh->id > maxid,
              "性质3: 还原存档后新建节点 id > 所有已还原 id");

        map_close(m);
    }

    // ---- 阶段 5: 再存再读, id 仍然稳定(回归: 链式存取不漂移) ----
    {
        MapConfig cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.base.max_workers = 0;
        map* m = map_open(TMP, &cfg);
        if (m) {
            map_node* n2 = add_text(map_root(m), "再存");
            CHECK(n2 != NULL, "二次编辑建节点");
            uint64_t keep = n2 ? n2->id : 0;
            map_save(m, TMP);
            map_close(m);

            map* m2 = map_open(TMP, &cfg);
            map_node* back = m2 ? find_by_text(m2, "再存") : NULL;
            CHECK(back != NULL && back->id == keep, "链式存取后 id 依然稳定");
            if (m2) map_close(m2);
        } else {
            CHECK(0, "map_open(二次存档)");
        }
    }

    remove(TMP);
    printf("== 结果: %s (%d 失败) ==\n", fails == 0 ? "PASS" : "FAIL", fails);
    return fails == 0 ? 0 : 1;
}
