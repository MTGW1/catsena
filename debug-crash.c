// debug-crash.c —— 最小复现: 定位 create 批量执行时的崩溃点
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "map.h"

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int idx = argc > 1 ? atoi(argv[1]) : 0;
    int N   = argc > 2 ? atoi(argv[2]) : 20;
    printf("start idx=%d N=%d\n", idx, N);

    MapConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.base.max_workers = 0;
    cfg.enable_index = idx;
    map* m = map_open(NULL, &cfg);
    printf("map_open → %s\n", m ? "ok" : "NULL");
    if (!m) return 2;

    char* buf = (char*)malloc(65536);
    char cmd[128];
    for (int i = 0; i < N; i++) {
        snprintf(cmd, sizeof cmd, "create \"\" 种子%d", i);
        int n = map_exec(m, cmd, buf, 65536);
        printf("  [%3d] rc=%d out=%.50s\n", i, n, n > 0 ? buf : "(none)");
    }
    printf("create 循环完成\n");

    char u[200];
    snprintf(cmd, sizeof cmd, "path 种子0");
    int n = map_exec(m, cmd, u, sizeof u);
    printf("path 种子0 → rc=%d out=%.70s\n", n, n > 0 ? u : "(none)");

    printf("map_close...\n");
    map_close(m);
    printf("done\n");
    return 0;
}
