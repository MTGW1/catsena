// verify-sha256.c —— 用标准向量验证内核内置 SHA256 实现
// 标准值: sha256("") / sha256("abc") / 长消息(跨块+填充边界)
#include <stdio.h>
#include <string.h>
#include "map.h"

static int fails = 0;
static void check(const char* label, const char* in, size_t len, const char* want) {
    char got[65];
    memset(got, 0, sizeof got);
    if (map_sha256(in, len, got) != 0) { printf("  FAIL %s: 调用失败\n", label); fails++; return; }
    if (strcmp(got, want) == 0) printf("  ok   %-22s %s\n", label, got);
    else { printf("  FAIL %-22s\n       got  %s\n       want %s\n", label, got, want); fails++; }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== 内核 SHA256 标准向量验证 ==\n");
    check("空串", "", 0,
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("abc", "abc", 3,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check("448bit(4b)", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    /* 55/56/57/63/64/65 字节: 覆盖填充边界(64 字节是原实现的越界点) */
    {
        char b55[56], b56[57], b57[58], b63[64], b64[65], b65[66];
        memset(b55, 'a', 55); b55[55] = 0;
        memset(b56, 'a', 56); b56[56] = 0;
        memset(b57, 'a', 57); b57[57] = 0;
        memset(b63, 'a', 63); b63[63] = 0;
        memset(b64, 'a', 64); b64[64] = 0;
        memset(b65, 'a', 65); b65[65] = 0;
        check("55×'a'", b55, 55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
        check("56×'a'", b56, 56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
        check("57×'a'", b57, 57, "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6");
        check("63×'a'", b63, 63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
        check("64×'a'", b64, 64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
        check("65×'a'", b65, 65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
    }
    printf("== %s ==\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
