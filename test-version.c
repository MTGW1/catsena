// test-version.c —— 版本接口自测
// 目的: 编译出来的库必须知道自己是谁; version.h 自动生成的那套东西不能是空的/对不上。
//
// 检查项:
//   1. 四个函数都返回非空
//   2. map_version() 与 CATSENA_VERSION 一致
//   3. map_build() 以 version 开头(0.1.0+...)
//   4. map_buildinfo() 里含 "catsena" 和版本号
//   5. map_abi() 等于 CATSENA_ABI, 且 > 0

#include <stdio.h>
#include <string.h>
#include "map.h"
#include "version.h"

static int fails = 0;
#define CHECK(cond, what) do {                                        \
    if (cond) printf("  ok   %s\n", what);                             \
    else { printf("  FAIL %s\n", what); fails++; }                     \
} while (0)

int main(void) {
    const char* v  = map_version();
    const char* b  = map_build();
    const char* bi = map_buildinfo();
    int abi = map_abi();

    printf("===== version =====\n");
    printf("  version   : %s\n", v ? v : "(null)");
    printf("  build     : %s\n", b ? b : "(null)");
    printf("  buildinfo : %s\n", bi ? bi : "(null)");
    printf("  abi       : %d\n", abi);

    CHECK(v && v[0], "map_version() 非空");
    CHECK(strcmp(v, CATSENA_VERSION) == 0, "map_version() == CATSENA_VERSION");
    CHECK(b && strncmp(b, v, strlen(v)) == 0, "map_build() 以 version 开头");
    CHECK(bi && strstr(bi, "catsena") != NULL, "buildinfo 含 catsena");
    CHECK(bi && strstr(bi, v) != NULL, "buildinfo 含版本号");
    CHECK(abi == CATSENA_ABI && abi > 0, "map_abi() == CATSENA_ABI > 0");
    CHECK(CATSENA_BUILD_ID[0] != '\0', "BUILD_ID 非空");

    printf("%s: %d 项失败\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
