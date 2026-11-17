// maptest.c —— catsena 自测(独立内核, 不依赖旧内核; 跨平台 Win32/POSIX)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "map.h"
#include "thirdparty/cjson/cJSON.h"

#ifdef _WIN32
#include <windows.h>   // platform.h(经 map.h)已带 _WIN32_WINNT 定义
#else
#include <unistd.h>   // usleep(POSIX 忙等)
#endif

static int g_evts = 0;
static int on_change(map* m, map_evt_kind evt, node* arg, void* ud) {
    (void)m; (void)evt; (void)arg; (void)ud;
    g_evts++;
    return 0;
}

/* 并发测试: done 回调计数(原子; Windows=Interlocked, POSIX=__atomic) */
struct ctest { volatile int* done; volatile int total; };
static void ctest_done(void* ud, int rc) {
    struct ctest* t = (struct ctest*)ud;
    (void)rc;
#ifdef _WIN32
    InterlockedIncrement((volatile LONG*)t->done);
#else
    __atomic_add_fetch(t->done, 1, __ATOMIC_SEQ_CST);
#endif
}

#define CHECK(cond, msg) do { if (cond) printf("  ok  %s\n", msg); \
    else { printf("  FAIL %s\n", msg); fails++; } } while (0)

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);   // 无缓冲: 挂死时也能看到卡点
    int fails = 0;
    printf("== catsena 自测 ==\n");

    // 0) 可选: 加载真实 .map 验证
    if (argc > 1) {
        MapConfig cfg0; memset(&cfg0, 0, sizeof(cfg0));
        cfg0.base.max_workers = 0;
        cfg0.enable_index = 1;
        map* m0 = map_open(argv[1], &cfg0);
        CHECK(m0 != NULL, "map_open(真实 .map)");
        if (m0) {
            char g[65536];
            map_exec(m0, "graphjson", g, sizeof(g));
            CHECK(strstr(g, "\"nodes\"") != NULL, "真实图 graphjson 输出");
            node* r = map_root(m0);
            CHECK(r != NULL && r->uuid != NULL, "真实图 root 可访问");
            node* hit = map_find_uuid(m0, r->uuid);
            CHECK(hit == r, "真实图索引命中 root");
            map_close(m0);
        }
        printf("== 结果: %s ==\n", fails == 0 ? "PASS" : "FAIL");
        return fails == 0 ? 0 : 1;
    }

    // 1) 打开空图
    MapConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.base.max_workers = 0;
    cfg.enable_index = 1;
    static const struct map_hooks hooks = { on_change, NULL };
    cfg.hooks = &hooks;
    map* m = map_open(NULL, &cfg);
    CHECK(m != NULL, "map_open(空图)");
    if (!m) return 1;

    // 2) 命令执行(create/attr/neighbor)
    char out[8192];
    int n = map_exec(m, "create \"\" 世界", out, sizeof(out));
    CHECK(n > 0 && strstr(out, "已建"), "map_exec create 世界");
    CHECK(g_evts >= 1, "on_change 钩子被调用");
    map_exec(m, "create 世界 角色A", out, sizeof(out));
    map_exec(m, "create 世界 角色B", out, sizeof(out));
    map_exec(m, "attr 世界/角色A rare 6", out, sizeof(out));
    map_exec(m, "neighbor 世界/角色A 世界/角色B", out, sizeof(out));

    // 3) 索引: find_by_uuid
    node* a = map_find_path(m, "世界/角色A");
    CHECK(a != NULL && a->uuid != NULL, "map_find_path 世界/角色A");
    if (a && a->uuid) {
        node* hit = map_find_uuid(m, a->uuid);
        CHECK(hit == a, "map_find_uuid 索引命中(O(1))");
    }
    const char* rare = a ? node_attr_get(a, "rare") : NULL;
    CHECK(rare != NULL && strcmp(rare, "6") == 0, "attr 属性可读(rare=6)");

    // 4) 命令后再查(懒重建)
    map_exec(m, "create 世界 角色C", out, sizeof(out));
    node* c = map_find_path(m, "世界/角色C");
    CHECK(c != NULL, "命令后路径查找(懒重建不破坏索引)");

    // 5) 子树收集
    node* w = map_find_path(m, "世界");
    node* arr[64];
    size_t cnt = map_collect_subtree(m, w, arr, 64);
    CHECK(cnt >= 4, "map_collect_subtree 收集(>=4 含邻居/边)");

    // 6) 索引计数
    char out2[4096];
    map_exec(m, "graphjson", out2, sizeof(out2));
    CHECK(strstr(out2, "角色A") != NULL, "graphjson 输出");

    // 7) map_submit 同步退化
    char out3[8192];
    int rc = map_submit(m, "ls 世界", out3, sizeof(out3), NULL, NULL);
    CHECK(rc > 0, "map_submit 同步模式执行");

    // 8) map_find_uuid 不存在
    CHECK(map_find_uuid(m, "0000000000000000000000000000000000000000000000000000000000000000") == NULL,
          "map_find_uuid 未命中返回 NULL");

    // 9) 引擎挂接
    MapOptions o; memset(&o, 0, sizeof(o));
    map_engine* e = engine_create(&o);
    CHECK(engine_attach_map(e, m) == 0, "engine_attach_map");
    rc = engine_submit(e, m, NULL);   // NULL cmd 应拒绝
    CHECK(rc == -1, "engine_submit 参数校验");
    map_cmd* cmd = (map_cmd*)calloc(1, sizeof(map_cmd));
    cmd->text = strdup("ls 世界");
    char out4[8192]; cmd->out = out4; cmd->outsz = sizeof(out4);
    rc = engine_submit(e, m, cmd);
    CHECK(rc > 0, "engine_submit 同步执行(经 map_exec)");
    engine_destroy(e);

    // 10) save → load 回环(序列化兼容)
    map_exec(m, "save _rt.map", out, sizeof(out));
    MapConfig cfg2; memset(&cfg2, 0, sizeof(cfg2));
    cfg2.base.max_workers = 0;
    cfg2.enable_index = 1;
    map* m2 = map_open("_rt.map", &cfg2);
    CHECK(m2 != NULL, "回环: 重新打开存档");
    if (m2) {
        node* a2 = map_find_path(m2, "世界/角色A");
        CHECK(a2 != NULL, "回环: 路径仍可解析");
        if (a2) {
            const char* r2 = node_attr_get(a2, "rare");
            CHECK(r2 && strcmp(r2, "6") == 0, "回环: attrs 保留");
            CHECK(a2->neighbors.count == 1, "回环: 邻居关系保留");
        }
        char g2[8192];
        map_exec(m2, "graphjson", g2, sizeof(g2));
        CHECK(strstr(g2, "角色C") != NULL, "回环: graphjson 完整");
        map_close(m2);
    }
    remove("_rt.map");

    // 11) hidden 语义归应用端(attrs.hidden=1 → graphjson 兼容输出 sys)
    map_exec(m, "attr 世界/角色B hidden 1", out, sizeof(out));
    map_exec(m, "graphjson", out, sizeof(out));
    CHECK(strstr(out, "\"sys\":1") != NULL, "attrs.hidden=1 → graphjson sys 兼容");
    map_exec(m, "attr 世界/角色B hidden -", out, sizeof(out));
    map_exec(m, "graphjson", out, sizeof(out));
    CHECK(strstr(out, "\"sys\":1") == NULL, "删除 hidden 后 sys 消失");

    // 12) 并发: worker 线程池 + 图级读写锁
    {
        MapOptions co; memset(&co, 0, sizeof(co));
        co.max_workers = 4;
        co.queue_depth = 0;
        map_engine* ce = engine_create(&co);
        CHECK(ce != NULL, "并发引擎创建(max_workers=4)");
        if (ce) {
            engine_attach_map(ce, m);
            volatile int done_cnt = 0;
            struct ctest ctx = { &done_cnt, 20 };
            static char outs[20][512];
            for (int i = 0; i < 20; i++) {
                char c[128];
                snprintf(c, sizeof(c), "create 世界 并发节点%d", i);
                map_cmd* mc = (map_cmd*)calloc(1, sizeof(map_cmd));
                mc->text = strdup(c);
                mc->out = outs[i];
                mc->outsz = sizeof(outs[i]);
                mc->done = ctest_done;
                mc->ud = &ctx;
                engine_submit(ce, m, mc);
            }
            // 等 20 个命令全部完成(忙等, 跨平台)
            for (int w = 0; w < 2000 && done_cnt < 20; w++) {
#ifdef _WIN32
                Sleep(5);
#else
                usleep(5000);
#endif
            }
            printf("  [diag] workers=%d/%d done_cnt=%d queue_len=%d\n",
                   ce->worker_count, ce->options.max_workers, (int)done_cnt, ce->queue_len);
            CHECK(done_cnt == 20, "20 个并发命令全部完成(线程池)");
            // 全部节点应已创建
            char g[16384];
            map_exec(m, "graphjson", g, sizeof(g));
            int hits = 0;
            for (int i = 0; i < 20; i++)
                if (strstr(g, "并发节点") != NULL) { hits++; break; }
            // 数节点: 统计 "并发节点" 出现次数(每个节点 name 一次)
            hits = 0;
            const char* p = g;
            while ((p = strstr(p, "并发节点")) != NULL) { hits++; p += 8; }
            CHECK(hits == 20, "并发创建的 20 节点全部在图中");
            // 清理
            for (int i = 0; i < 20; i++) {
                char c[128];
                snprintf(c, sizeof(c), "del 世界/并发节点%d", i);
                map_exec(m, c, out, sizeof(out));
            }
            engine_destroy(ce);
        }
    }

    // 13) 视图锚定根(应用端权限语义的基础能力)
    {
        node* w = map_find_path(m, "世界");
        CHECK(w != NULL, "锚定: 定位 世界 节点");
        CHECK(map_set_view(m, w) == 0, "map_set_view(世界)");
        CHECK(map_view(m) == w, "map_view 返回锚定根");
        // 锚定后 "角色A" 相对锚定根解析
        node* a = map_find_path(m, "角色A");
        CHECK(a != NULL, "锚定后相对路径解析(角色A)");
        char g[8192];
        map_exec(m, "graphjson", g, sizeof(g));
        cJSON* j = cJSON_Parse(g);
        CHECK(j != NULL, "锚定后 graphjson 可解析");
        if (j) {
            const char* root_uuid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "root"));
            CHECK(root_uuid && w && strcmp(root_uuid, w->uuid) == 0, "graphjson root = 锚定根 uuid");
            cJSON_Delete(j);
        }
        map_set_view(m, NULL);
        CHECK(map_view(m) == NULL, "恢复整图视图");
    }

    map_close(m);
    printf("== 结果: %s ==\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
