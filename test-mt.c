// test-mt.c —— catsena 多线程并发测试(竞态 + 伸缩性)
// ---------------------------------------------------------------------------
// 补 maptest 未覆盖的并发面:
//   A) 并发读: N 读线程同时按 uuid 寻址(读锁共享, 但索引 rebuild 会写共享索引)
//   B) 读写混合: 1 写者改图 + N-1 读者并发读(锁语义/悬垂指针)
//   C) 并发写: N 线程同时 create(同一父), 验证无丢节点/无损坏
//   D) 伸缩性: 同一读负载 1 线程 vs N 线程吞吐(测 SRWLock 是否真并行)
// 用法: test-mt.exe [线程数] [每线程操作数] [enable_index 0/1] [图路径]
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map.h"
#include "mapworker.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#endif

#define MAXT   16
#define BUFSZ  65536
#define NSEED  500          /* 预置节点数 */

/* ---- 原子计数 ---- */
#ifdef _WIN32
#define AINC(p) InterlockedIncrement((volatile LONG*)(p))
#else
#define AINC(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#endif
static volatile long g_hit, g_miss, g_wrong, g_short;

/* ---- 计时 ---- */
static double now_s(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

typedef struct {
    map*  m;
    int   id;
    int   role;        /* 0=读者 1=写者 */
    long  ops;
    char  uuids[8][65];/* 读目标 uuid(预取, 避免测试内开销) */
    int   nuuid;
    double secs;
    long  local_ok, local_bad;
} targ;

/* ---- 读者: 反复按 uuid 寻址(每次命令后索引标脏 → 下次读触发 rebuild) ---- */
static MAP_THREAD_RET reader_fn(LPVOID p) {
    targ* a = (targ*)p;
    char* buf = (char*)malloc(BUFSZ);
    char cmd[128];
    long ok = 0, bad = 0;
    double t0 = now_s();
    for (long i = 0; i < a->ops; i++) {
        int k = (int)(i % a->nuuid);
        snprintf(cmd, sizeof cmd, "path uuid:%s", a->uuids[k]);
        int n = map_exec(a->m, cmd, buf, BUFSZ);
        if (n <= 0 || buf[0] == '(') { AINC(&g_miss); bad++; continue; }   /* "(none)" = 未命中 */
        if (strncmp(buf, a->uuids[k], 64) == 0) { AINC(&g_hit); ok++; }
        else { AINC(&g_wrong); bad++; }
        (void)bad;
    }
    a->secs = now_s() - t0;
    a->local_ok = ok; a->local_bad = bad;
    free(buf);
    MAP_THREAD_RETURN(0);
}

/* ---- 写者: set 改属性(写锁排他; 触发 refresh_uuid) ---- */
static MAP_THREAD_RET writer_fn(LPVOID p) {
    targ* a = (targ*)p;
    char* buf = (char*)malloc(BUFSZ);
    char cmd[128];
    long ok = 0, bad = 0;
    double t0 = now_s();
    for (long i = 0; i < a->ops; i++) {
        int k = (int)(i % a->nuuid);
        snprintf(cmd, sizeof cmd, "path uuid:%s", a->uuids[k]);
        map_exec(a->m, cmd, buf, BUFSZ);
        /* 写: 只改"写区"节点(不碰读目标的 data, 否则内容指纹变化会让读目标 uuid 失效) */
        snprintf(cmd, sizeof cmd, "set 写区%ld v%ld", i % 200, i);
        int n = map_exec(a->m, cmd, buf, BUFSZ);
        if (n > 0) ok++; else bad++;
    }
    a->secs = now_s() - t0;
    a->local_ok = ok; a->local_bad = bad;
    free(buf);
    MAP_THREAD_RETURN(0);
}

/* ---- 并发写: 各线程建自己的节点 ---- */
typedef struct { map* m; int id; long ops; long made; } warg;
static MAP_THREAD_RET creator_fn(LPVOID p) {
    warg* a = (warg*)p;
    char* buf = (char*)malloc(BUFSZ);
    char cmd[128];
    long made = 0;
    for (long i = 0; i < a->ops; i++) {
        snprintf(cmd, sizeof cmd, "create 并发区 T%d_%ld", a->id, i);
        int n = map_exec(a->m, cmd, buf, BUFSZ);
        if (n > 0 && strstr(buf, "已建") != NULL) made++;
    }
    a->made = made;
    free(buf);
    MAP_THREAD_RETURN(0);
}

static int heap_ok(void) {
#ifdef _WIN32
    return HeapValidate(GetProcessHeap(), 0, NULL) ? 1 : 0;
#else
    return 1;
#endif
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int T    = argc > 1 ? atoi(argv[1]) : 8;
    long OPS = argc > 2 ? atol(argv[2]) : 2000;
    int idx  = argc > 3 ? atoi(argv[3]) : 1;
    const char* path = argc > 4 ? argv[4] : NULL;
    if (T < 1) T = 1;
    if (T > MAXT) T = MAXT;

    printf("== catsena 多线程并发测试 ==\n");
    printf("线程=%d 每线程操作=%ld 索引=%s 图=%s\n", T, OPS, idx ? "开" : "关", path ? path : "(内存)");

    MapConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.base.max_workers = 0;      /* 同步执行: 由测试自己起线程, 直接压 map_exec 的锁 */
    cfg.enable_index = idx;
    map* m = map_open(path, &cfg);
    if (m == NULL) { printf("map_open 失败\n"); return 2; }

    char* buf = (char*)malloc(BUFSZ);
    char cmd[128];

    /* ---- 预置: 500 个种子节点 + 200 个写区节点 ---- */
    double t0 = now_s();
    for (int i = 0; i < NSEED; i++) {
        snprintf(cmd, sizeof cmd, "create \"\" 种子%d", i);
        map_exec(m, cmd, buf, BUFSZ);
    }
    for (int i = 0; i < 200; i++) {   /* 写者专用(改它不影响读目标指纹) */
        snprintf(cmd, sizeof cmd, "create \"\" 写区%d", i);
        map_exec(m, cmd, buf, BUFSZ);
    }
    printf("[预置] %d 节点 用时 %.1fms\n", NSEED + 200, (now_s() - t0) * 1000);

    /* 预取 uuid 供读线程使用 */
    char uuids[8][65];
    int nu = 0;
    for (int k = 0; k < 8; k++) {
        int i = k * (NSEED / 8);
        snprintf(cmd, sizeof cmd, "path 种子%d", i);
        int n = map_exec(m, cmd, buf, BUFSZ);
        if (n > 0 && buf[0] != '(') {
            memcpy(uuids[nu], buf, 64);
            uuids[nu][64] = 0;
            nu++;
        }
    }
    printf("[预取] %d 个 uuid(读目标)\n", nu);
    if (nu == 0) { printf("无法取 uuid, 中止\n"); map_close(m); return 2; }

    targ args[MAXT];
    map_thread_t th[MAXT];
    int fails = 0;

    /* ================= A) 并发读(索引 rebuild 竞态面) ================= */
    printf("\n[A] 并发读: %d 线程 × %ld 次 uuid 寻址\n", T, OPS);
    g_hit = g_miss = g_wrong = 0;
    for (int i = 0; i < T; i++) {
        memset(&args[i], 0, sizeof args[i]);
        args[i].m = m; args[i].id = i; args[i].role = 0; args[i].ops = OPS;
        args[i].nuuid = nu;
        memcpy(args[i].uuids, uuids, sizeof uuids);
        th[i] = map_thread_start(reader_fn, &args[i]);
    }
    for (int i = 0; i < T; i++) map_thread_wait(&th[i]);
    double read_secs = 0;
    for (int i = 0; i < T; i++) if (args[i].secs > read_secs) read_secs = args[i].secs;
    long total_read = (long)T * OPS;
    printf("  命中=%ld 未命中=%ld uuid不符=%ld | 用时 %.3fs | 吞吐 %.0f ops/s\n",
           g_hit, g_miss, g_wrong, read_secs, read_secs > 0 ? total_read / read_secs : 0);
    if (g_hit == total_read) printf("  ok  全部命中(索引一致)\n");
    else { printf("  FAIL 命中 %ld/%ld —— 索引竞态或查找失败\n", g_hit, total_read); fails++; }
    if (!heap_ok()) { printf("  FAIL 堆已损坏(HeapValidate)\n"); fails++; }
    else printf("  ok  堆完整\n");

    /* ================= B) 读写混合 ================= */
    printf("\n[B] 读写混合: 1 写者 + %d 读者 × %ld\n", T - 1 > 0 ? T - 1 : 1, OPS);
    g_hit = g_miss = g_wrong = 0;
    int nread = T - 1 > 0 ? T - 1 : 1;
    for (int i = 0; i <= nread; i++) {
        memset(&args[i], 0, sizeof args[i]);
        args[i].m = m; args[i].id = i;
        args[i].ops = OPS;
        args[i].nuuid = nu;
        memcpy(args[i].uuids, uuids, sizeof uuids);
        args[i].role = (i == 0) ? 1 : 0;   /* 0 号是写者 */
        th[i] = map_thread_start(i == 0 ? writer_fn : reader_fn, &args[i]);
    }
    for (int i = 0; i <= nread; i++) map_thread_wait(&th[i]);
    long wok = args[0].local_ok;
    printf("  写成功=%ld/%ld | 读命中=%ld 未命中=%ld uuid不符=%ld\n", wok, OPS, g_hit, g_miss, g_wrong);
    if (wok == OPS) printf("  ok  写全部生效\n"); else { printf("  FAIL 写丢失\n"); fails++; }
    if (g_wrong == 0) printf("  ok  无脏读(uuid 全对)\n"); else { printf("  FAIL uuid 不符(悬垂/竞态)\n"); fails++; }
    if (!heap_ok()) { printf("  FAIL 堆已损坏\n"); fails++; } else printf("  ok  堆完整\n");

    /* ================= C) 并发写 ================= */
    printf("\n[C] 并发写: %d 线程各 create %ld 个\n", T, OPS / 10 + 1);
    warg wa[MAXT];
    long per = OPS / 10 + 1;
    snprintf(cmd, sizeof cmd, "create \"\" 并发区");
    map_exec(m, cmd, buf, BUFSZ);   /* 容器只建一次(重名会被拒) */
    for (int i = 0; i < T; i++) {
        wa[i].m = m; wa[i].id = i; wa[i].ops = per; wa[i].made = 0;
        th[i] = map_thread_start(creator_fn, &wa[i]);
    }
    for (int i = 0; i < T; i++) map_thread_wait(&th[i]);
    long made = 0;
    for (int i = 0; i < T; i++) made += wa[i].made;
    printf("  建节点成功=%ld / 期望=%ld\n", made, (long)T * per);
    if (made == (long)T * per) printf("  ok  并发创建无丢失\n");
    else { printf("  FAIL 创建丢失 %ld 个\n", (long)T * per - made); fails++; }
    if (!heap_ok()) { printf("  FAIL 堆已损坏\n"); fails++; } else printf("  ok  堆完整\n");

    /* ================= D) 伸缩性: 1 线程 vs T 线程 ================= */
    printf("\n[D] 伸缩性: 同一读负载(每线程 %ld 次)\n", OPS);
    /* 单线程基线 */
    g_hit = g_miss = g_wrong = 0;
    memset(&args[0], 0, sizeof args[0]);
    args[0].m = m; args[0].ops = OPS; args[0].nuuid = nu;
    memcpy(args[0].uuids, uuids, sizeof uuids);
    th[0] = map_thread_start(reader_fn, &args[0]);
    map_thread_wait(&th[0]);
    double t1 = args[0].secs;
    double tp1 = OPS / t1;
    printf("  1 线程:  %.3fs  %.0f ops/s\n", t1, tp1);
    /* T 线程 */
    g_hit = g_miss = g_wrong = 0;
    for (int i = 0; i < T; i++) {
        memset(&args[i], 0, sizeof args[i]);
        args[i].m = m; args[i].ops = OPS; args[i].nuuid = nu;
        memcpy(args[i].uuids, uuids, sizeof uuids);
        th[i] = map_thread_start(reader_fn, &args[i]);
    }
    for (int i = 0; i < T; i++) map_thread_wait(&th[i]);
    double tT = 0;
    for (int i = 0; i < T; i++) if (args[i].secs > tT) tT = args[i].secs;
    double tpT = ((long)T * OPS) / tT;
    printf("  %d 线程: %.3fs  %.0f ops/s\n", T, tT, tpT);
    printf("  加速比: %.2fx (理想 %.1fx)\n", tpT / tp1, (double)T);
    printf("  伸缩效率: %.0f%%\n", 100.0 * (tpT / tp1) / T);

    map_close(m);
    free(buf);
    printf("\n== 结果: %s (堆%s) ==\n", fails == 0 ? "PASS" : "FAIL", heap_ok() ? "完整" : "损坏");
    return fails == 0 ? 0 : 1;
}
