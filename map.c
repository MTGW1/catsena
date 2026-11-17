// map.c —— catsena 独立图内核实现(不依赖旧内核 graph.c/ui.c)
// ============================================================================
// 纯净模型: node(数据+attrs+关系) + 命令层(图操作, 无业务语义) + 序列化。
// 时间线/页面/权限等语义由应用端经 attrs 识别(见 THEMAP_ARCH.md)。
// 序列化格式与旧 .map 兼容(同构 JSON) —— 可直接读现有存档(时间字段忽略)。
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#include "map.h"
#include "mapworker.h"
#include "version.h"                  // 自动生成(见 tools/genversion.mjs), 别手改

#include "thirdparty/cjson/cJSON.h"   // 内嵌 cJSON(MIT, 来源见 DEPENDENCIES.md)

/* ================= 小工具 ================= */

// ============================================================
// 内嵌 SHA-256 实现（RFC 6234，公共域）
// 流式: 字节缓冲 64B, 满块即压缩; bitlen 累计已喂入的比特数
// ============================================================
#define SHA256_BLOCK_SIZE 32
typedef struct {
    uint32_t h[8];
    uint64_t bitlen;      // 累计比特数(用于填充)
    uint8_t  buf[64];     // 未满块的字节缓冲
    size_t   buflen;      // 缓冲内字节数(0..63)
} sha256_ctx;

// SHA256 常量表
static uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

// 初始化 SHA256 上下文
static void sha256_init(sha256_ctx *c) {
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(c->h, iv, sizeof iv);
    c->bitlen = 0;
    c->buflen = 0;
    memset(c->buf, 0, sizeof c->buf);
}


#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))      // 32-bit 循环右移
#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))               // 选择函数
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z))) // 多数函数
#define SIG0(x) (ROTR(x,7) ^ ROTR(x,18) ^ ((x) >> 3))        // 32-bit σ0
#define SIG1(x) (ROTR(x,17) ^ ROTR(x,19) ^ ((x) >> 10))      // 32-bit σ1
#define SIG2(x) (ROTR(x,2) ^ ROTR(x,13) ^ ROTR(x,22))        // 32-bit Σ0
#define SIG3(x) (ROTR(x,6) ^ ROTR(x,11) ^ ROTR(x,25))        // 32-bit Σ1

// SHA256 压缩一个 64 字节块(大端装载 w[0..15])
static void sha256_process_block(sha256_ctx *c, const uint8_t *blk) {
    uint32_t w[64], i, t1, t2;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)blk[i*4] << 24) | ((uint32_t)blk[i*4+1] << 16) |
               ((uint32_t)blk[i*4+2] << 8) | (uint32_t)blk[i*4+3];
    for (; i < 64; i++) w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];
    uint32_t a = c->h[0], b = c->h[1], e = c->h[4], f = c->h[5];
    uint32_t g = c->h[6], h = c->h[7], d = c->h[3], c0 = c->h[2];
    for (i = 0; i < 64; i++) {
        t1 = h + SIG3(e) + CH(e,f,g) + sha256_k[i] + w[i];
        t2 = SIG2(a) + MAJ(a,b,c0);
        h = g; g = f; f = e; e = d + t1;
        d = c0; c0 = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += c0; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

// 追加数据(流式): 填缓冲, 满 64 字节即压缩
static void sha256_update(sha256_ctx *c, const void *data, size_t len) {
    if (c == NULL || data == NULL || len == 0) return;
    const uint8_t *p = (const uint8_t*)data;
    c->bitlen += (uint64_t)len * 8u;
    while (len > 0) {
        size_t room = 64 - c->buflen;
        size_t n = (len < room) ? len : room;
        memcpy(c->buf + c->buflen, p, n);
        c->buflen += n;
        p += n;
        len -= n;
        if (c->buflen == 64) { sha256_process_block(c, c->buf); c->buflen = 0; }
    }
}

// 完成 SHA256 计算并输出结果(标准填充: 0x80, 补 0 至 56 mod 64, 大端 64 位比特长度)
static void sha256_final(sha256_ctx *c, uint8_t *out) {
    uint64_t bits = c->bitlen;
    uint8_t pad = 0x80, zero = 0x00, lenbuf[8];
    sha256_update(c, &pad, 1);
    while (c->buflen != 56) sha256_update(c, &zero, 1);   // buflen∈[0,63] → 必达 56
    for (int i = 0; i < 8; i++) lenbuf[i] = (uint8_t)(bits >> (56 - i * 8));
    sha256_update(c, lenbuf, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4]     = (uint8_t)(c->h[i] >> 24);
        out[i*4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i*4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i*4 + 3] = (uint8_t)(c->h[i]);
    }
}

// 计算数据的 SHA256 并输出为 hex 字符串
int sha256(const char* data, size_t len, char* out_hex) {
    if (!data || !out_hex) return -1;
    sha256_ctx ctx;
    uint8_t hash[32];
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, hash);
    for (size_t i = 0; i < 32; i++) {
        sprintf(out_hex + i*2, "%02x", hash[i]);
    }
    out_hex[64] = '\0';
    return 0;
}

// 往指针列表里塞入指针, 塞满了就扩一倍
static int arr_push(ptr_array* a, void* p) {
    if (a == NULL || p == NULL) return -1;
    if (a->count >= a->capacity) {
        size_t_ nc = a->capacity == 0 ? 8 : a->capacity * 2;
        void** ni = (void**)realloc((void*)a->items, nc * sizeof(void*));
        if (ni == NULL) return -1;
        a->items = ni;
        a->capacity = nc;
    }
    a->items[a->count++] = p;
    return 0;
}

// 在指针列表里找指针, 找到返回索引, 找不到返回 -1
static ptrdiff_t arr_find(const ptr_array* a, const void* p) {
    for (size_t_ i = 0; i < a->count; i++)
        if (a->items[i] == p) return (ptrdiff_t)i;
    return -1;
}

// 清空指针列表, 释放内存
static void arr_clear(ptr_array* a) {
    if (a == NULL) return;
    free((void*)a->items);
    a->items = NULL; a->count = 0; a->capacity = 0;
}

//往sha256_ctx里喂入一个64位整数(小端)
static void feed_u64(sha256_ctx* c, uint64_t v) {
    unsigned char b[8];
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)(v >> (8 * i));
    sha256_update(c, b, 8);
}

//往sha256_ctx里喂入一个字符串(前面加分隔符0x1F)
static void feed_str(sha256_ctx* c, const char* s) {
    if (s == NULL) return;
    unsigned char sep = 0x1F;
    sha256_update(c, &sep, 1);
    sha256_update(c, s, strlen(s));
}

/* ================= 指针哈希集(O(1) 去重) =================
   用途: 替代"线性扫描去重"的数组 —— 否则收集/DFS 退化为 O(N²);
   原来 find_uuid_dfs 还用栈上 visited[8192] 固定数组, 图 >8192 节点会越界崩溃。 */

typedef struct { node** slots; size_t cap, count; } node_set;

static size_t ptr_hash(const void* p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return (size_t)x;
}

static int set_grow(node_set* s) {
    size_t ncap = s->cap ? s->cap * 2 : 64;
    node** ns = (node**)calloc(ncap, sizeof(node*));
    if (ns == NULL) return -1;
    for (size_t i = 0; i < s->cap; i++) {
        node* n = s->slots[i];
        if (n == NULL) continue;
        size_t j = ptr_hash(n) & (ncap - 1);
        while (ns[j] != NULL) j = (j + 1) & (ncap - 1);
        ns[j] = n;
    }
    free((void*)s->slots);
    s->slots = ns; s->cap = ncap;
    return 0;
}

// 返回 1 = 新增, 0 = 已存在(含分配失败)
static int set_add(node_set* s, node* n) {
    if (s == NULL || n == NULL) return 0;
    if (s->cap == 0) { if (set_grow(s) != 0) return 0; }
    if ((s->count + 1) * 4 >= s->cap * 3) { if (set_grow(s) != 0) return 0; }
    size_t j = ptr_hash(n) & (s->cap - 1);
    while (s->slots[j] != NULL) {
        if (s->slots[j] == n) return 0;
        j = (j + 1) & (s->cap - 1);
    }
    s->slots[j] = n; s->count++;
    return 1;
}

static void set_free(node_set* s) { free((void*)s->slots); s->slots = NULL; s->cap = s->count = 0; }

// 比较两个node的uuid指针, 用于排序(保证指纹与集合插入顺序无关)
static int uuid_ptr_cmp(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

// 喂入一组引用节点的 uuid(排序后逐个喂入 → 集合语义)
static void feed_ref_uuids(sha256_ctx* c, const ptr_array* arr) {
    if (arr == NULL || arr->count == 0) return;
    const char** ids = (const char**)malloc(arr->count * sizeof(char*));
    if (ids == NULL) return;
    size_t m = 0;
    for (size_t_ i = 0; i < arr->count; i++) {
        node* r = (node*)arr->items[i];
        if (r != NULL && r->uuid != NULL) ids[m++] = r->uuid;
    }
    if (m > 1) qsort((void*)ids, m, sizeof(char*), uuid_ptr_cmp);
    for (size_t i = 0; i < m; i++) feed_str(c, ids[i]);
    free((void*)ids);
}

// 属性集: 按 key(再 value) 排序后喂入 —— 非空才计入
static int attr_ptr_cmp(const void* a, const void* b) {
    const node_attr* x = *(const node_attr* const*)a;
    const node_attr* y = *(const node_attr* const*)b;
    int r = strcmp(x->key ? x->key : "", y->key ? y->key : "");
    if (r != 0) return r;
    return strcmp(x->value ? x->value : "", y->value ? y->value : "");
}
static void feed_attrs(sha256_ctx* c, const ptr_array* attrs) {
    if (attrs == NULL || attrs->count == 0) return;
    const node_attr** arr = (const node_attr**)malloc(attrs->count * sizeof(void*));
    if (arr == NULL) return;
    size_t m = 0;
    for (size_t_ i = 0; i < attrs->count; i++) {
        const node_attr* at = (const node_attr*)attrs->items[i];
        if (at != NULL) arr[m++] = at;
    }
    if (m > 1) qsort((void*)arr, m, sizeof(void*), attr_ptr_cmp);
    for (size_t i = 0; i < m; i++) { feed_str(c, arr[i]->key); feed_str(c, arr[i]->value); }
    free((void*)arr);
}

/* ================= node 生命周期 ================= */

/* 进程级 id 分配器(文件作用域: 载入存档时要把游标推到已还原 id 之上) */
static volatile uint64_t g_next_id = 1;

// 分配节点(不挂任何关系); data 外部所有(owns_data=0)
node* map_node_new(data_kind dtype, void* data, size_t data_len) {
    node* n = (node*)calloc(1, sizeof(node));
    if (n == NULL) return NULL;
    n->id = map_id_alloc(&g_next_id);
    n->dtype = dtype;
    n->data = data;
    n->alive = 1;
    if (dtype == DATA_TEXT && data != NULL && data_len == 0)
        data_len = strlen((const char*)data);
    n->data_len = data_len;
    if (data != NULL && data_len > 0) {
        sha256_ctx ctx;
        uint8_t hash[32];
        sha256_init(&ctx);
        sha256_update(&ctx, data, data_len);
        sha256_final(&ctx, hash);
        char* hex = (char*)malloc(65);
        if (hex) {
            for (size_t i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hash[i]);
            hex[64] = 0;
        }
        n->sha256 = hex;
    }
    n->uuid = node_compute_uuid(n);
    return n;
}

// 声明 used_id 已被占用(载入存档还原 id 后调用): 保证后续 map_node_new 不与之撞车
void map_node_reserve_id(uint64_t used_id) {
    map_id_bump(&g_next_id, used_id);
}

// 计算 uuid 指纹(除 id/uuid 外全部字段; attrs 非空才计入)
// 单向设计: 只把"父引用"的身份喂进指纹(父身份 → 子唯一), 反向(子/邻居/边引用)只计数 ——
//   否则父子互相依赖 → 指纹永远无法收敛。副作用: 增删子节点不改变父的 uuid(身份稳定)。
char* node_compute_uuid(const node* n) {
    if (n == NULL) return NULL;
    sha256_ctx ctx;
    sha256_init(&ctx);
    // 数据内容
    if (n->data != NULL && n->data_len > 0) {
        unsigned char sep = 0x1F;
        unsigned char lenb[4] = {
            (unsigned char)(n->data_len & 0xFF),
            (unsigned char)((n->data_len >> 8) & 0xFF),
            (unsigned char)((n->data_len >> 16) & 0xFF),
            (unsigned char)((n->data_len >> 24) & 0xFF)
        };
        sha256_update(&ctx, &sep, 1);
        sha256_update(&ctx, lenb, 4);
        sha256_update(&ctx, n->data, n->data_len);
    }
    feed_u64(&ctx, n->data_len);
    feed_str(&ctx, n->sha256);
    feed_u64(&ctx, (uint64_t)n->dtype);
    // 关系: 父引用计入身份(区分同名节点/不同位置), 其余只计数
    feed_u64(&ctx, (uint64_t)n->parents.count);
    feed_u64(&ctx, (uint64_t)n->neighbors.count);
    feed_u64(&ctx, (uint64_t)n->edges.count);
    feed_ref_uuids(&ctx, &n->parents);
    // 边节点: 两端**集合**的身份(边 = 关系的实体化, 端点即其身份)。
    //   feed_ref_uuids 会排序 → 同一组端点无论插入顺序如何都是同一个身份。
    //   兼容: 旧档只有单个 edge_a/edge_b, 读进来就是两端各 1 个 → 旧边指纹不变。
    feed_ref_uuids(&ctx, &n->edge_as);
    feed_ref_uuids(&ctx, &n->edge_bs);
    // 属性(node_attr, 排序后喂入)
    feed_attrs(&ctx, &n->attrs);
    uint8_t hash[32];
    sha256_final(&ctx, hash);
    char* out = (char*)malloc(65);
    if (out == NULL) return NULL;
    for (size_t i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", hash[i]);
    out[64] = 0;
    return out;
}

// 重算单个节点指纹; 返回 1 = uuid 发生变化
static int rehash_node(node* n) {
    char* nu = node_compute_uuid(n);
    if (nu == NULL) return 0;
    int changed = (n->uuid == NULL || strcmp(n->uuid, nu) != 0);
    free(n->uuid);
    n->uuid = nu;
    return changed;
}

// 重算节点 uuid 并向下传播(父 uuid 变 → 子指纹含父引用, 必须跟着重算)
static void refresh_uuid_down(node* n, node_set* seen) {
    if (n == NULL) return;
    if (!set_add(seen, n)) return;            // 环安全
    if (!rehash_node(n)) return;              // 身份未变 → 子树无需重算
    for (size_t_ i = 0; i < n->childs.count; i++)
        refresh_uuid_down((node*)n->childs.items[i], seen);
    for (size_t_ i = 0; i < n->edges.count; i++)
        refresh_uuid_down((node*)n->edges.items[i], seen);   // 边节点: 端点身份变 → 边身份变
}

// 重算节点 uuid(结构变化后调用: 链接建立/断开 → 指纹含引用身份需更新)。
// 指纹含父引用 → 自身身份变化时子孙需级联重算(见 refresh_uuid_down)。
void refresh_uuid(node* n) {
    if (n == NULL || n->uuid == NULL) return;
    node_set seen;
    memset(&seen, 0, sizeof seen);
    refresh_uuid_down(n, &seen);
    set_free(&seen);
}

/* 改写节点载荷(data 交由节点自持) —— 指纹的两个输入必须同步:
     sha256 = SHA256(data)        指纹里喂了 data_len 和 sha256
     uuid   = 重算(并向下传播)
   直接写 n->data 而不更新这两样 = 身份与内容不符: 本次会话看不出来, 但
   存档 → 重开时 refresh_uuid 会按真实内容重算 → 整棵子树 uuid 漂移。
   (实测踩过: map_open 挂图名时漏了 sha256, 5 节点的图重开后 5/5 uuid 全变) */
static void node_adopt_data(node* n, char* data, size_t len) {
    if (n == NULL) return;
    if (n->owns_data && n->data != NULL) free(n->data);
    n->data = data;
    n->data_len = len;
    n->owns_data = 1;
    free(n->sha256);
    n->sha256 = NULL;
    if (data != NULL && len > 0) {
        sha256_ctx ctx;
        uint8_t hash[32];
        sha256_init(&ctx);
        sha256_update(&ctx, data, len);
        sha256_final(&ctx, hash);
        char* hex = (char*)malloc(65);
        if (hex != NULL) {
            for (size_t i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hash[i]);
            hex[64] = 0;
            n->sha256 = hex;
        }
    }
    refresh_uuid(n);
}

/* ================= attrs ================= */
// 设置节点属性(语义键值对, 内核只存不解释)
int node_attr_set(node* n, const char* key, const char* value) {
    if (n == NULL || key == NULL || key[0] == '\0') return 1;
    int del = (value == NULL || value[0] == '\0');
    for (size_t_ i = 0; i < n->attrs.count; i++) {
        node_attr* a = (node_attr*)n->attrs.items[i];
        if (a == NULL || a->key == NULL) continue;
        if (strcmp(a->key, key) == 0) {
            if (del) {
                free(a->key); free(a->value); free(a);
                for (size_t_ j = i; j < n->attrs.count - 1; j++) n->attrs.items[j] = n->attrs.items[j+1];
                n->attrs.count--;
            } else {
                char* nv = strdup(value);
                if (nv == NULL) return 2;
                free(a->value);
                a->value = nv;
            }
            // free(n->conn);
            free(n->uuid);
            n->uuid = node_compute_uuid(n);
            return 0;
        }
    }
    if (del) return 0;
    node_attr* a = (node_attr*)calloc(1, sizeof(node_attr));
    if (a == NULL) return 2;
    a->key = strdup(key);
    a->value = strdup(value);
    if (a->key == NULL || a->value == NULL || arr_push(&n->attrs, a) != 0) {
        free(a->key); free(a->value); free(a);
        return 2;
    }
    // free(n->conn);
    // n->conn = NULL;
    free(n->uuid);
    n->uuid = node_compute_uuid(n);
    return 0;
}

// 获取节点属性值
const char* node_attr_get(const node* n, const char* key) {
    if (n == NULL || key == NULL) return NULL;
    for (size_t_ i = 0; i < n->attrs.count; i++) {
        node_attr* a = (node_attr*)n->attrs.items[i];
        if (a && a->key && strcmp(a->key, key) == 0) return a->value;
    }
    return NULL;
}

// 释放节点属性表
static void node_attrs_free(node* n) {
    for (size_t_ i = 0; i < n->attrs.count; i++) {
        node_attr* a = (node_attr*)n->attrs.items[i];
        if (a) { free(a->key); free(a->value); free(a); }
    }
    arr_clear(&n->attrs);
}

/* ================= 关系 ================= */
// 建立父子关系(双向), 并刷新 uuid 指纹(含连接引用)
int link_parent_child(node* parent, node* child) {
    if (parent == NULL || child == NULL || parent == child) return 1;
    if (arr_find(&parent->childs, child) >= 0) return 0;
    if (arr_push(&parent->childs, child) != 0) return 2;
    if (arr_push(&child->parents, parent) != 0) { parent->childs.count--; return 2; }
    // uuid 是内容指纹(含连接引用): 结构变化 → 重算, 同名节点因父不同而指纹不同
    refresh_uuid(parent);
    refresh_uuid(child);
    return 0;
}

// 断开父子关系(双向), 并刷新 uuid 指纹(含连接引用)
int unlink_parent_child(node* parent, node* child) {
    if (parent == NULL || child == NULL) return 1;
    int ok = 0;
    ptrdiff_t i = arr_find(&parent->childs, child);
    if (i >= 0) { for (size_t_ j = (size_t)i; j < parent->childs.count - 1; j++) parent->childs.items[j] = parent->childs.items[j+1]; parent->childs.count--; ok = 1; }
    i = arr_find(&child->parents, parent);
    if (i >= 0) { for (size_t_ j = (size_t)i; j < child->parents.count - 1; j++) child->parents.items[j] = child->parents.items[j+1]; child->parents.count--; ok = 1; }
    if (ok) { refresh_uuid(parent); refresh_uuid(child); }
    return ok ? 0 : 1;
}

// 连接邻居(双向), 并刷新 uuid 指纹(含连接引用)
int link_neighbor(node* a, node* b) {
    if (a == NULL || b == NULL || a == b) return 1;
    if (arr_find(&a->neighbors, b) >= 0) return 0;
    if (arr_push(&a->neighbors, b) != 0) return 2;
    if (arr_push(&b->neighbors, a) != 0) { a->neighbors.count--; return 2; }
    refresh_uuid(a);
    refresh_uuid(b);
    return 0;
}

// 断开邻居(双向), 并刷新 uuid 指纹(含连接引用)
int unlink_neighbor(node* a, node* b) {
    if (a == NULL || b == NULL) return 1;
    int ok = 0;
    ptrdiff_t i = arr_find(&a->neighbors, b);
    if (i >= 0) { for (size_t_ j = (size_t_)i; j < a->neighbors.count - 1; j++) a->neighbors.items[j] = a->neighbors.items[j+1]; a->neighbors.count--; ok = 1; }
    i = arr_find(&b->neighbors, a);
    if (i >= 0) { for (size_t_ j = (size_t_)i; j < b->neighbors.count - 1; j++) b->neighbors.items[j] = b->neighbors.items[j+1]; b->neighbors.count--; ok = 1; }
    if (ok) { refresh_uuid(a); refresh_uuid(b); }
    return ok ? 0 : 1;
}

// 节点名的一行摘要(data 第一行, 截断) —— 命令回执用, 别把整篇正文回显出来
static void brief_name(const node* n, char* buf, size_t cap) {
    size_t k = 0;
    const char* s = (n != NULL && n->data != NULL) ? (const char*)n->data : "?";
    if (cap == 0) return;
    for (; k + 1 < cap && s[k] != 0 && s[k] != '\n'; k++) buf[k] = s[k];
    buf[k] = 0;
}

// 边角色判定(内容判定式): 端点是核心依据, 不依赖 dtype —— 载荷类型(TEXT/FILE)与
//   拓扑角色正交。两端**集合都非空**即视为边节点(1→N / N×M 都算)。
int node_is_edge(const node* n) {
    return n != NULL && n->edge_as.count > 0 && n->edge_bs.count > 0;
}

// 端点侧: 0 = 主体(A, 发出侧), 1 = 客体(B, 指向侧)
static ptr_array* edge_side(node* e, int side) {
    return (side == 0) ? &e->edge_as : &e->edge_bs;
}

/* 往边的某一端加一个节点(幂等: 已在集合里直接算成功)。
   同时把边登记到该节点的 edges 列表 —— 双向引用必须同时成立。
   与 link_parent_child 一致: 结构真变了就自己刷 uuid(身份是内容指纹)。 */
int edge_end_add(node* e, node* n, int side) {
    if (e == NULL || n == NULL) return -1;
    ptr_array* set = edge_side(e, side);
    int changed = 0;
    if (arr_find(set, n) < 0) {
        if (arr_push(set, n) != 0) return -1;
        changed = 1;
    }
    if (arr_find(&n->edges, e) < 0 && arr_push(&n->edges, e) != 0) return -1;
    if (changed) { refresh_uuid(e); refresh_uuid(n); }
    return 0;
}

/* 从某一端摘掉一个节点。返回 1 = 原本就不在集合里。
   只有当这个节点**不再是这条边的任何一端**时, 才把边从它的 edges 列表里摘掉
   (自指边 / 同一节点既是 A 端又是 B 端的情况)。 */
int edge_end_del(node* e, node* n, int side) {
    if (e == NULL || n == NULL) return 1;
    ptr_array* set = edge_side(e, side);
    ptrdiff_t i = arr_find(set, n);
    if (i < 0) return 1;
    for (size_t_ j = (size_t_)i; j + 1 < set->count; j++) set->items[j] = set->items[j + 1];
    set->count--;
    if (arr_find(&e->edge_as, n) < 0 && arr_find(&e->edge_bs, n) < 0) {
        ptrdiff_t k = arr_find(&n->edges, e);
        if (k >= 0) {
            for (size_t_ j = (size_t_)k; j + 1 < n->edges.count; j++) n->edges.items[j] = n->edges.items[j + 1];
            n->edges.count--;
        }
    }
    refresh_uuid(e);
    refresh_uuid(n);
    return 0;
}

// 建边(空载荷: 载荷类型 TEXT, 由端点判定角色; 后续可用 set/API 填充)
node* edge_connect(node* a, node* b) {
    return edge_connect_with(a, b, NULL, 0);
}

// 前向声明: edge_connect_with 里用到(定义在其下方)
int edge_attach(node* e, node* a, node* b);

// 建边并带载荷(data 外部所有, 同 map_node_new 语义): 文字/文件均可。
// 内容寻址幂等: 同向、同端点、**无载荷无属性**的边再次创建 → 返回已存在的那条。
//   理由: 这种边的内容指纹完全相同(uuid 相同), 两个对象共享一个 uuid 会让索引只认其一
//   (find_uuid 命中哪个不确定)。既然内容相同就是一回事, 创建时直接复用。
//   要并行边 → 给每条加标题(内容不同即身份不同), 视觉上的弧度错开见 RENDER_RULES §2.4。
//   注意: 载入存档不走这里(见 json_to_bare + edge_attach) —— 存档里的边有自己的载荷,
//   一律要按原样重建, 否则并行边会被误当重复合并。
//   幂等只对"两端各 1 个"的简单边生效; 多端边请用 edge_end_add 显式加端。
node* edge_connect_with(node* a, node* b, void* data, size_t data_len) {
    if (a == NULL || b == NULL) return NULL;
    if ((data == NULL || data_len == 0)) {
        for (size_t_ i = 0; i < a->edges.count; i++) {
            node* old = (node*)a->edges.items[i];
            if (old != NULL && node_is_edge(old)
                && old->edge_as.count == 1 && old->edge_bs.count == 1
                && old->edge_as.items[0] == a && old->edge_bs.items[0] == b
                && (old->data == NULL || old->data_len == 0) && old->attrs.count == 0)
                return old;   // 等价的空边已存在
        }
    }
    node* e = map_node_new(DATA_TEXT, data, data_len);
    if (e == NULL) return NULL;
    if (edge_attach(e, a, b) != 0) { map_node_free(e); return NULL; }
    // 边节点指纹含端点关系: 结构变化 → 重算(两端点 edges 引用计入指纹)
    refresh_uuid(e);
    refresh_uuid(a);
    if (b != a) refresh_uuid(b);
    return e;
}

// 把"已经造好的节点"登记成一条边(不新建): 载入存档时用 —— 载荷/属性由存档决定。
// 只做结构: 两端各挂 1 个节点。多端请再调 edge_end_add。
// uuid 重算由调用方决定(它可能要覆盖存档 uuid)。
int edge_attach(node* e, node* a, node* b) {
    if (e == NULL || a == NULL || b == NULL) return -1;
    if (edge_end_add(e, a, 0) != 0) return -1;
    if (edge_end_add(e, b, 1) != 0) return -1;
    return 0;
}

// 删除节点: 摘除全部关系 + 递归释放子树(不释放邻居/边自身, 只摘连接)
void map_node_free(node* n) {
    if (n == NULL || !n->alive) return;
    n->alive = 0;
    /* 若 n 是边节点(或只是"装了端点的半成品"): 先把 n 从每个端点的 edges 数组摘掉,
       否则端点持有野指针。判定用"任一端非空"而不是 node_is_edge —— 一次失败的
       半成品边也要能干净释放。 */
    if (n->edge_as.count > 0 || n->edge_bs.count > 0) {
        for (int side = 0; side < 2; side++) {
            ptr_array* set = edge_side(n, side);
            for (size_t_ i = 0; i < set->count; i++) {
                node* en = (node*)set->items[i];
                if (en == NULL) continue;
                ptrdiff_t k = arr_find(&en->edges, n);
                if (k >= 0) {
                    for (size_t_ j = (size_t_)k; j + 1 < en->edges.count; j++) en->edges.items[j] = en->edges.items[j + 1];
                    en->edges.count--;
                    refresh_uuid(en);   // 端点 edges 变化 → 指纹重算
                }
            }
        }
    }
    // 摘 childs(递归释放子树)
    while (n->childs.count > 0) {
        node* c = (node*)n->childs.items[0];
        ptrdiff_t i = arr_find(&c->parents, n);
        if (i >= 0) { for (size_t_ j = (size_t_)i; j < c->parents.count - 1; j++) c->parents.items[j] = c->parents.items[j+1]; c->parents.count--; }
        for (size_t_ j = 0; j < n->childs.count - 1; j++) n->childs.items[j] = n->childs.items[j+1];
        n->childs.count--;
        map_node_free(c);
    }
    // 摘邻居
    while (n->neighbors.count > 0) {
        node* nb = (node*)n->neighbors.items[0];
        unlink_neighbor(n, nb);
    }
    // 摘边节点: 把 n 从每条边的**两端集合**里去掉。
    //   某端被摘空 → 那条边不再是边(关系少了一头就不存在了) → 整条释放。
    //   先把自己从 n->edges 里摘掉, 保证循环一定推进(即使边还活着)。
    while (n->edges.count > 0) {
        node* e = (node*)n->edges.items[0];
        if (e == NULL) { n->edges.count--; continue; }
        ptrdiff_t k = arr_find(&n->edges, e);
        if (k >= 0) {
            for (size_t_ j = (size_t_)k; j + 1 < n->edges.count; j++) n->edges.items[j] = n->edges.items[j + 1];
            n->edges.count--;
        }
        int touched = 0;
        for (int side = 0; side < 2; side++) {
            ptr_array* set = edge_side(e, side);
            ptrdiff_t j = arr_find(set, n);
            if (j >= 0) {
                for (size_t_ m = (size_t_)j; m + 1 < set->count; m++) set->items[m] = set->items[m + 1];
                set->count--;
                touched = 1;
            }
        }
        if (!node_is_edge(e)) {
            map_node_free(e);          // 端点摘空 → 边消失(它会把自己从剩余端点摘干净)
        } else if (touched) {
            refresh_uuid(e);           // 端点集变了 → 边身份变(其余端点只计数, 不级联)
        }
    }
    if (n->owns_data) free((void*)n->data);
    free(n->sha256);
    // free(n->conn);
    free(n->uuid);
    node_attrs_free(n);
    arr_clear(&n->neighbors);
    arr_clear(&n->childs);
    arr_clear(&n->parents);
    arr_clear(&n->edges);
    arr_clear(&n->edge_as);
    arr_clear(&n->edge_bs);
    free(n);
}

/* ================= 查找 ================= */

// 按 data(TEXT)精确匹配直接子节点
static node* find_child_by_data(node* parent, const char* name) {
    if (parent == NULL || name == NULL) return NULL;
    for (size_t_ i = 0; i < parent->childs.count; i++) {
        node* c = (node*)parent->childs.items[i];
        if (c && c->dtype == DATA_TEXT && c->data && strcmp((const char*)c->data, name) == 0)
            return c;
    }
    return NULL;
}

// 路径解析: "a/b/c"; 空串 = root
node* find_node_by_path(node* root, const char* path) {
    if (root == NULL) return NULL;
    if (path == NULL || path[0] == '\0') return root;
    node* cur = root;
    char* buf = strdup(path);
    if (buf == NULL) return NULL;
    char* save = NULL;
    for (char* seg = strtok_r(buf, "/", &save); seg != NULL; seg = strtok_r(NULL, "/", &save)) {
        if (seg[0] == '\0') continue;
        cur = find_child_by_data(cur, seg);
        if (cur == NULL) break;
    }
    free(buf);
    return cur;
}

// 深搜按 uuid 找(seen 防环 —— neighbors 双向/edges 环可能成环; 哈希集无规模上限)
static node* find_uuid_dfs(node* n, const char* uuid, node_set* seen) {
    if (n == NULL) return NULL;
    if (!set_add(seen, n)) return NULL;      // 已访问(环)
    if (n->uuid && strcmp(n->uuid, uuid) == 0) return n;
    for (size_t_ i = 0; i < n->childs.count; i++) {
        node* hit = find_uuid_dfs((node*)n->childs.items[i], uuid, seen);
        if (hit) return hit;
    }
    for (size_t_ i = 0; i < n->neighbors.count; i++) {
        node* hit = find_uuid_dfs((node*)n->neighbors.items[i], uuid, seen);
        if (hit) return hit;
    }
    // 边节点挂在端点 edges 数组(不在 child 树) → 必须遍历才能 uuid 寻址
    for (size_t_ i = 0; i < n->edges.count; i++) {
        node* hit = find_uuid_dfs((node*)n->edges.items[i], uuid, seen);
        if (hit) return hit;
    }
    return NULL;
}

/* ================= uuid 哈希索引(懒重建) ================= */

// 哈希表条目
typedef struct idx_entry { char* uuid; node* nd; } idx_entry;
typedef struct uuid_index {
    idx_entry* slots;
    size_t cap, count;
    volatile int dirty;
    map_rwlock_t lock;     /* 读共享(查找)/写排他(重建): 读命令持共享图锁, 但重建会写表 → 自保 */
} uuid_index;

// 哈希函数: djb2
static size_t str_hash(const char* s) {
    size_t h = 5381;
    while (s && *s) h = ((h << 5) + h) + (unsigned char)*s++;   // NULL 防御(clang-analyzer)
    return h;
}

// 哈希表构建/释放/查找/插入
static uuid_index* idx_new(void) {
    uuid_index* ix = (uuid_index*)calloc(1, sizeof(uuid_index));
    if (ix == NULL) return NULL;
    ix->cap = 256;
    ix->slots = (idx_entry*)calloc(ix->cap, sizeof(idx_entry));
    if (ix->slots == NULL) { free(ix); return NULL; }
    map_rwlock_init(&ix->lock);
    ix->dirty = 1;
    return ix;
}

// 释放哈希表
static void idx_free(uuid_index* ix) {
    if (ix == NULL) return;
    for (size_t i = 0; i < ix->cap; i++) free(ix->slots[i].uuid);
    free((void*)ix->slots);
    free(ix);
}

// 清空条目(保留已分配的槽位; 重建前调用 —— 否则删除过的节点会留下悬垂指针)
static void idx_clear(uuid_index* ix) {
    for (size_t i = 0; i < ix->cap; i++) {
        free(ix->slots[i].uuid);
        ix->slots[i].uuid = NULL;
        ix->slots[i].nd = NULL;
    }
    ix->count = 0;
}

// 插入条目(若已存在则覆盖)
static void idx_put(uuid_index* ix, const char* uuid, node* nd) {
    if ((ix->count + 1) * 4 >= ix->cap * 3) {
        size_t ncap = ix->cap * 2;
        idx_entry* ns = (idx_entry*)calloc(ncap, sizeof(idx_entry));
        if (ns == NULL) return;
        for (size_t i = 0; i < ix->cap; i++) {
            if (ix->slots[i].uuid == NULL) continue;
            size_t j = str_hash(ix->slots[i].uuid) & (ncap - 1);
            while (ns[j].uuid) j = (j + 1) & (ncap - 1);
            ns[j] = ix->slots[i];
        }
        free((void*)ix->slots);
        ix->slots = ns;
        ix->cap = ncap;
    }
    size_t j = str_hash(uuid) & (ix->cap - 1);
    while (ix->slots[j].uuid) {
        if (strcmp(ix->slots[j].uuid, uuid) == 0) { ix->slots[j].nd = nd; return; }
        j = (j + 1) & (ix->cap - 1);
    }
    ix->slots[j].uuid = strdup(uuid);
    ix->slots[j].nd = nd;
    ix->count++;
}

// 查找条目(若不存在返回 NULL)
static node* idx_get(uuid_index* ix, const char* uuid) {
    if (ix == NULL || ix->count == 0) return NULL;
    size_t j = str_hash(uuid) & (ix->cap - 1);
    for (size_t p = 0; p < ix->cap; p++) {
        if (ix->slots[j].uuid == NULL) return NULL;
        if (strcmp(ix->slots[j].uuid, uuid) == 0) return ix->slots[j].nd;
        j = (j + 1) & (ix->cap - 1);
    }
    return NULL;
}

// 收集整图(DFS child+邻居+边) —— bag 保持插入顺序, seen 做 O(1) 去重
typedef struct node_bag { node** items; size_t count, cap; node_set seen; } node_bag;
static int bag_add(node_bag* b, node* nd) {
    if (nd == NULL) return 0;
    if (!set_add(&b->seen, nd)) return 0;       // 已收集
    if (b->count >= b->cap) {
        size_t nc = b->cap == 0 ? 64 : b->cap * 2;
        node** ni = (node**)realloc((void*)b->items, nc * sizeof(node*));
        if (ni == NULL) return -1;
        b->items = ni;
        b->cap = nc;
    }
    b->items[b->count++] = nd;
    return 0;
}

// 清空 bag, 释放内存
static void bag_clear(node_bag* b) { free((void*)b->items); b->items = NULL; b->count = b->cap = 0; set_free(&b->seen); }

// 深搜收集整图(DFS child+邻居+边, 指针去重)
static int collect_dfs(node* n, node_bag* bag) {
    if (n == NULL) return 0;
    if (bag_add(bag, n) != 0) return -1;
    // 自身邻居/边(锚点根的场景: 根自身挂的 neighbor/edge 也要收集)
    for (size_t_ k = 0; k < n->neighbors.count; k++) bag_add(bag, (node*)n->neighbors.items[k]);
    for (size_t_ k = 0; k < n->edges.count; k++) bag_add(bag, (node*)n->edges.items[k]);
    // 边节点的边(边指向边): 边节点自身挂的边也要递归收集
    for (size_t_ k = 0; k < n->edges.count; k++) {
        node* e = (node*)n->edges.items[k];
        if (e != NULL && e->edges.count > 0) collect_dfs(e, bag);
    }
    for (size_t_ i = 0; i < n->childs.count; i++) {
        node* c = (node*)n->childs.items[i];
        if (collect_dfs(c, bag) != 0) return -1;
        for (size_t_ k = 0; k < c->neighbors.count; k++) bag_add(bag, (node*)c->neighbors.items[k]);
        for (size_t_ k = 0; k < c->edges.count; k++) bag_add(bag, (node*)c->edges.items[k]);
        // 子节点的边节点的边(边指向边, 深层)
        for (size_t_ k = 0; k < c->edges.count; k++) {
            node* e = (node*)c->edges.items[k];
            if (e != NULL && e->edges.count > 0) collect_dfs(e, bag);
        }
    }
    return 0;
}

// 重建 uuid 索引(整图 DFS 收集, O(1) 去重)
static void idx_rebuild(map* m, uuid_index* ix) {
    idx_clear(ix);                     // 先清陈旧条目(防悬垂指针 + 表膨胀)
    node_bag bag = {0};
    if (collect_dfs(m->root, &bag) == 0)
        for (size_t i = 0; i < bag.count; i++)
            if (bag.items[i]->uuid) idx_put(ix, bag.items[i]->uuid, bag.items[i]);
    bag_clear(&bag);
    ix->dirty = 0;
}

/* ================= 序列化(格式兼容旧 .map) ================= */

// 序列化 uuid 数组(指针数组 → uuid 字符串数组)
static void add_uuid_arr(cJSON* o, const char* name, const ptr_array* arr) {
    if (arr->count == 0) return;
    cJSON* ja = cJSON_AddArrayToObject(o, name);
    for (size_t_ i = 0; i < arr->count; i++) {
        const node* ne = (const node*)arr->items[i];
        if (ne && ne->uuid) cJSON_AddItemToArray(ja, cJSON_CreateString(ne->uuid));
    }
}

// 节点转 JSON 对象(递归子树)
static cJSON* node_to_json(const node* n) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double)n->id);
    cJSON_AddNumberToObject(o, "dtype", (double)(int)n->dtype);
    if (n->uuid) cJSON_AddStringToObject(o, "uuid", n->uuid);
    if (n->data && n->data_len > 0) {
        // data_hex(兼容二进制)
        char* hex = (char*)malloc(n->data_len * 2 + 1);
        if (hex) {
            for (size_t_ i = 0; i < n->data_len; i++) sprintf(hex + i * 2, "%02x", ((unsigned char*)n->data)[i]);
            hex[n->data_len * 2] = 0;
            cJSON_AddStringToObject(o, "data_hex", hex);
            free(hex);
        }
    }
    if (n->attrs.count > 0) {
        cJSON* ja = cJSON_AddObjectToObject(o, "attrs");
        for (size_t_ i = 0; i < n->attrs.count; i++) {
            node_attr* a = (node_attr*)n->attrs.items[i];
            if (a && a->key && a->key[0]) cJSON_AddStringToObject(ja, a->key, a->value ? a->value : "");
        }
    }
    add_uuid_arr(o, "parents_uuid", &n->parents);
    add_uuid_arr(o, "neighbors_uuid", &n->neighbors);
    add_uuid_arr(o, "edges_uuid", &n->edges);
    if (node_is_edge(n)) {
        /* 两端是集合: 单端沿用旧的 edge_a_uuid/edge_b_uuid(老读档器仍能用),
           多端再加 *_uuids 数组 —— 读档器优先数组、回退单值。 */
        const node* a0 = (const node*)n->edge_as.items[0];
        const node* b0 = (const node*)n->edge_bs.items[0];
        cJSON_AddStringToObject(o, "edge_a_uuid", a0 && a0->uuid ? a0->uuid : "");
        cJSON_AddStringToObject(o, "edge_b_uuid", b0 && b0->uuid ? b0->uuid : "");
        if (n->edge_as.count > 1) add_uuid_arr(o, "edge_a_uuids", &n->edge_as);
        if (n->edge_bs.count > 1) add_uuid_arr(o, "edge_b_uuids", &n->edge_bs);
    }
    if (n->childs.count > 0) {
        cJSON* ja = cJSON_AddArrayToObject(o, "children");
        for (size_t_ i = 0; i < n->childs.count; i++) {
            const node* c = (const node*)n->childs.items[i];
            cJSON* co = node_to_json(c);
            if (co) cJSON_AddItemToArray(ja, co);
        }
    }
    return o;
}

// 从 hex 还原 data(TEXT 带 \0)
static void* hex_to_bytes(const char* hex, size_t* out_len) {
    size_t hl = strlen(hex);
    if (hl % 2) return NULL;
    size_t bl = hl / 2;
    unsigned char* b = (unsigned char*)malloc(bl + 1);
    if (!b) return NULL;
    for (size_t i = 0; i < bl; i++) {
        unsigned int v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) { free(b); return NULL; }
        b[i] = (unsigned char)v;
    }
    b[bl] = 0;
    if (out_len) *out_len = bl;
    return b;
}

// uuid 注册表(第二遍连线用)
typedef struct uuid_map { const char* uuid; node* nd; } uuid_map;

/* 解析一条边存档的**两端 uuid 集合** → 已解析的节点指针数组。
   读的顺序: 数组键(edge_a_uuids/edge_b_uuids) 优先, 回退旧的单值键(edge_a_uuid/edge_b_uuid)。
   返回 0 = 两端都解析成功(as/ac, bs/bc 由调用方 free); 1 = 还有端点没建出来(留给下一遍,
   处理"边指向边"这种端点本身也是边的情形)。 */
static int edge_ends_resolve(cJSON* eo, const uuid_map* reg, size_t cnt,
                             const uuid_map* ereg, size_t ecnt,
                             node*** as, size_t_* ac, node*** bs, size_t_* bc) {
    node** out[2] = {NULL, NULL};
    size_t_ n[2] = {0, 0};
    int ok = 0;
    for (int side = 0; side < 2 && !ok; side++) {
        const char* akey = (side == 0) ? "edge_a_uuids" : "edge_b_uuids";
        const char* okey = (side == 0) ? "edge_a_uuid"  : "edge_b_uuid";
        cJSON* arr = cJSON_GetObjectItem(eo, akey);
        int is_arr = cJSON_IsArray(arr);
        const char* one = cJSON_GetStringValue(cJSON_GetObjectItem(eo, okey));
        int m = is_arr ? cJSON_GetArraySize(arr) : ((one != NULL && one[0] != '\0') ? 1 : 0);
        if (m <= 0) break;                                  // 这端没端点 → 这张牌不是合法边
        node** v = (node**)calloc((size_t)m, sizeof(node*));
        if (v == NULL) break;
        int good = 1;
        for (int i = 0; i < m && good; i++) {
            const char* u = is_arr ? cJSON_GetStringValue(cJSON_GetArrayItem(arr, i)) : one;
            node* nd = NULL;
            if (u != NULL && u[0] != '\0') {
                for (size_t k = 0; k < cnt && nd == NULL; k++)
                    if (reg[k].uuid && strcmp(reg[k].uuid, u) == 0) nd = reg[k].nd;
                for (size_t k = 0; k < ecnt && nd == NULL; k++)
                    if (ereg[k].uuid && strcmp(ereg[k].uuid, u) == 0) nd = ereg[k].nd;
            }
            if (nd == NULL) good = 0; else v[i] = nd;
        }
        if (!good) { free((void*)v); break; }
        out[side] = v; n[side] = (size_t_)m;
        if (side == 1) ok = 1;
    }
    if (!ok) { free((void*)out[0]); free((void*)out[1]); return 1; }
    *as = out[0]; *ac = n[0]; *bs = out[1]; *bc = n[1];
    return 0;
}

/* JSON 对象 → 裸节点(不带父子/邻居关系): dtype / data / id / uuid / attrs 全按存档还原。
   树节点和**边节点**共用它 —— 边在存档里是平铺的(顶层 edges 区段), 必须同样把
   载荷和属性带回来, 否则"存档 → 重开"会丢边名与边属性(实测: 5 条边重开后全是空边)。 */
static node* json_to_bare(cJSON* obj) {
    if (obj == NULL || !cJSON_IsObject(obj)) return NULL;
    cJSON* jdtype = cJSON_GetObjectItem(obj, "dtype");
    cJSON* jdata = cJSON_GetObjectItem(obj, "data_hex");
    cJSON* juuid = cJSON_GetObjectItem(obj, "uuid");
    cJSON* jid = cJSON_GetObjectItem(obj, "id");
    cJSON* jres = cJSON_GetObjectItem(obj, "reserved");
    cJSON* jattrs = cJSON_GetObjectItem(obj, "attrs");

    data_kind dtype = DATA_TEXT;
    if (jdtype && cJSON_IsNumber(jdtype)) dtype = (data_kind)(int)cJSON_GetNumberValue(jdtype);
    size_t dlen = 0;
    void* dptr = NULL;
    const char* dhex = (jdata && cJSON_IsString(jdata)) ? cJSON_GetStringValue(jdata) : NULL;
    if (dhex && dhex[0]) dptr = hex_to_bytes(dhex, &dlen);

    node* nd = map_node_new(dtype, dptr, dlen);
    if (nd == NULL) { free(dptr); return NULL; }
    nd->owns_data = 1;
    /* 还原持久句柄 id(不进 uuid 指纹, 可安全覆盖):
       视图态/书签/撤销以 id 为 key, 必须跨存档稳定。
       老存档没有 id 字段 → 保留 map_node_new 现分配的, 行为不变。 */
    if (jid && cJSON_IsNumber(jid) && cJSON_GetNumberValue(jid) >= 1.0) {
        double dv = cJSON_GetNumberValue(jid);
        if (dv < 9.0e15) {                 /* 防溢出(uint64 精确整数上限 2^53) */
            nd->id = (uint64_t)dv;
            map_node_reserve_id(nd->id);   /* 游标推到已还原 id 之上 */
        }
    }
    // 旧档迁移: reserved 字段 → attrs.hidden=1(隐藏语义归应用端, 内核无此字段)
    if (jres && cJSON_IsNumber(jres) && cJSON_GetNumberValue(jres) != 0)
        node_attr_set(nd, "hidden", "1");
    if (juuid && cJSON_IsString(juuid) && cJSON_GetStringValue(juuid)[0]) {
        char* su = strdup(cJSON_GetStringValue(juuid));   // 保留原 uuid(关系引用稳定)
        if (su) { free(nd->uuid); nd->uuid = su; }
    }
    if (jattrs && cJSON_IsObject(jattrs)) {
        cJSON* jk = NULL;
        cJSON_ArrayForEach(jk, jattrs) {
            if (!jk->string) continue;
            const char* v = (cJSON_IsString(jk) && cJSON_GetStringValue(jk)) ? cJSON_GetStringValue(jk) : "";
            node_attr* a = (node_attr*)calloc(1, sizeof(node_attr));
            if (!a) continue;
            a->key = strdup(jk->string);
            a->value = strdup(v);
            if (!a->key || !a->value || arr_push(&nd->attrs, a) != 0) { free(a->key); free(a->value); free(a); }
        }
    }
    return nd;
}

// 递归 JSON → node 树(第一遍: 建节点/父子关系, 注册 uuid)
static node* json_to_node(cJSON* obj, node* parent, uuid_map* reg, size_t cap, size_t* cnt) {
    if (obj == NULL || !cJSON_IsObject(obj)) return NULL;
    cJSON* juuid = cJSON_GetObjectItem(obj, "uuid");
    cJSON* jch = cJSON_GetObjectItem(obj, "children");
    node* nd = json_to_bare(obj);
    if (nd == NULL) return NULL;
    if (parent && link_parent_child(parent, nd) != 0) { map_node_free(nd); return NULL; }
    if (reg && cnt && *cnt < cap && nd->uuid) {
        reg[*cnt].uuid = cJSON_GetStringValue(juuid);
        reg[*cnt].nd = nd;
        (*cnt)++;
    }
    if (jch && cJSON_IsArray(jch)) {
        int sz = cJSON_GetArraySize(jch);
        for (int i = 0; i < sz; i++) json_to_node(cJSON_GetArrayItem(jch, i), nd, reg, cap, cnt);
    }
    return nd;
}

// 递归计数 JSON 树节点(含顶层)
static int json_count_flat(cJSON* o) {
    if (o == NULL || !cJSON_IsObject(o)) return 0;
    int n = 1;
    cJSON* jch = cJSON_GetObjectItem(o, "children");
    if (jch && cJSON_IsArray(jch)) {
        int sz = cJSON_GetArraySize(jch);
        for (int i = 0; i < sz; i++) n += json_count_flat(cJSON_GetArrayItem(jch, i));
    }
    return n;
}

// 第二遍连线: 对每个节点的 neighbors_uuid 重建无向邻接(edges 由顶层区段重建)
static void link_from_json(cJSON* obj, uuid_map* reg, size_t cnt) {
    if (obj == NULL || !cJSON_IsObject(obj)) return;
    const char* self_uuid = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "uuid"));
    if (self_uuid) {
        // 找本节点
        node* nd = NULL;
        for (size_t k = 0; k < cnt; k++)
            if (reg[k].uuid && strcmp(reg[k].uuid, self_uuid) == 0) { nd = reg[k].nd; break; }
        if (nd) {
            cJSON* jn = cJSON_GetObjectItem(obj, "neighbors_uuid");
            if (jn && cJSON_IsArray(jn)) {
                int sz = cJSON_GetArraySize(jn);
                for (int i = 0; i < sz; i++) {
                    const char* nu = cJSON_GetStringValue(cJSON_GetArrayItem(jn, i));
                    if (!nu) continue;
                    for (size_t k = 0; k < cnt; k++)
                        if (reg[k].uuid && strcmp(reg[k].uuid, nu) == 0) {
                            link_neighbor(nd, reg[k].nd);
                            break;
                        }
                }
            }
        }
    }
    cJSON* jch = cJSON_GetObjectItem(obj, "children");
    if (jch && cJSON_IsArray(jch)) {
        int sz = cJSON_GetArraySize(jch);
        for (int i = 0; i < sz; i++) link_from_json(cJSON_GetArrayItem(jch, i), reg, cnt);
    }
}

// 载入时的 uuid 去重辅助: (uuid, node) 记录, 按 uuid 排序找重复组
typedef struct { const char* u; node* nd; } uuid_rec;
static int uuid_rec_cmp(const void* a, const void* b) {
    return strcmp(((const uuid_rec*)a)->u, ((const uuid_rec*)b)->u);
}

// 载入后 uuid 去重: 同父同内容(同名)的节点指纹必然相同 —— 索引以 uuid 为键,
// 重复会让后面的节点无法被寻址。给重复者打 dedup 属性(attrs 计入指纹)使其唯一。
static int load_dedup(uuid_rec* rec, size_t m) {
    if (rec == NULL || m < 2) return 0;
    qsort(rec, m, sizeof(uuid_rec), uuid_rec_cmp);
    int tagged = 0;
    size_t i = 0;
    while (i < m) {
        size_t j = i + 1;
        while (j < m && strcmp(rec[j].u, rec[i].u) == 0) j++;
        if (j - i > 1) {                        // [i, j) 同 uuid
            for (size_t k = i + 1; k < j; k++) {
                char v[24];
                snprintf(v, sizeof v, "%u", (unsigned)(k - i));
                if (node_attr_set(rec[k].nd, "dedup", v) == 0) {
                    refresh_uuid(rec[k].nd);
                    tagged++;
                }
            }
        }
        i = j;
    }
    return tagged;
}

// 加载 .map(明文 JSON; gzip 暂不支持, 后续加)
static int map_load_file(map* m, const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) return -1;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz <= 0 || fsz > 64 * 1024 * 1024) { fclose(f); return -1; }
    char* js = (char*)malloc((size_t)fsz + 1);
    if (js == NULL) { fclose(f); return -1; }
    if (fread(js, 1, (size_t)fsz, f) != (size_t)fsz) { fclose(f); free(js); return -1; }
    fclose(f);
    js[fsz] = 0;

    cJSON* root = cJSON_Parse(js);
    free(js);
    if (root == NULL) return -1;

    // 计数 → 建树 → 按 uuid 引用连 neighbors/edges
    size_t total = (size_t)json_count_flat(root);
    cJSON* je = cJSON_GetObjectItem(root, "edges");
    size_t edge_cnt = (je && cJSON_IsArray(je)) ? (size_t)cJSON_GetArraySize(je) : 0;
    size_t cap = total + edge_cnt + 1;
    uuid_map* reg = (uuid_map*)malloc(cap * sizeof(uuid_map));
    if (reg == NULL) { cJSON_Delete(root); return -1; }
    size_t cnt = 0;
    node* r = json_to_node(root, NULL, reg, cap, &cnt);

    // 连线: 顶层 edges 区段 + 节点内 neighbors_uuid(第二遍)
    link_from_json(root, reg, cnt);   // neighbors(递归全树)

    // 顶层 edges 区段(两遍: 先建全部边节点, 再补"边指向边"——端点可能是边节点, 需先存在)
    if (je && cJSON_IsArray(je)) {
        int sz = cJSON_GetArraySize(je);
        // 第一遍: 建所有端点能解析的边(普通端点), 记录新边节点 uuid→node(边指向边的端点用)
        // 查找表: reg(children 树) + 已建边节点
        size_t edge_cnt2 = 0;
        uuid_map* ereg = (uuid_map*)calloc((size_t)sz + 1, sizeof(uuid_map));
        if (ereg != NULL) {
            for (int i = 0; i < sz; i++) {
                cJSON* eo = cJSON_GetArrayItem(je, i);
                node** ra = NULL; node** rb = NULL; size_t_ ac = 0, bc = 0;
                if (edge_ends_resolve(eo, reg, cnt, ereg, 0, &ra, &ac, &rb, &bc) != 0) continue;
                // 按存档内容重建边(载荷 + 属性 + uuid 全部还原), 而不是造一条空边再硬改 uuid
                node* e = json_to_bare(eo);
                if (e != NULL) {
                    for (size_t_ k = 0; k < ac && e != NULL; k++)
                        if (edge_end_add(e, ra[k], 0) != 0) { map_node_free(e); e = NULL; }
                    for (size_t_ k = 0; k < bc && e != NULL; k++)
                        if (edge_end_add(e, rb[k], 1) != 0) { map_node_free(e); e = NULL; }
                }
                if (e != NULL) {
                    char* su = strdup(cJSON_GetStringValue(cJSON_GetObjectItem(eo, "uuid")));
                    if (su) { free(e->uuid); e->uuid = su; }
                    refresh_uuid(e);   // 覆盖存档 uuid 后用新端点重算
                    // 端点身份变了 → 端点也要重算, 否则索引里的旧 uuid 会失配
                    for (size_t_ k = 0; k < ac; k++) refresh_uuid(ra[k]);
                    for (size_t_ k = 0; k < bc; k++) if (arr_find(&e->edge_as, rb[k]) < 0) refresh_uuid(rb[k]);
                }
                const char* eu = cJSON_GetStringValue(cJSON_GetObjectItem(eo, "uuid"));
                if (e != NULL && eu != NULL) {
                    ereg[edge_cnt2].uuid = strdup(eu);
                    ereg[edge_cnt2].nd = e;
                    edge_cnt2++;
                }
                free((void*)ra); free((void*)rb);
            }
            // 第二遍+: 端点未解析(边指向边/自指边) → 从已建边节点里找; 循环多遍直到无新增
            // (自指边"认识→认识"依赖自身先建, 需多遍收敛)
            int progress = 1;
            while (progress && edge_cnt2 < (size_t)sz) {
                progress = 0;
                for (int i = 0; i < sz; i++) {
                    cJSON* eo = cJSON_GetArrayItem(je, i);
                    // 已建(跳过): 检查是否已在 ereg
                    int done = 0;
                    const char* eu = cJSON_GetStringValue(cJSON_GetObjectItem(eo, "uuid"));
                    for (size_t k = 0; k < edge_cnt2; k++) if (ereg[k].uuid && eu && strcmp(ereg[k].uuid, eu) == 0) { done = 1; break; }
                    if (done) continue;
                    node** ra = NULL; node** rb = NULL; size_t_ ac = 0, bc = 0;
                    if (edge_ends_resolve(eo, reg, cnt, ereg, edge_cnt2, &ra, &ac, &rb, &bc) != 0) continue;
                    node* e = json_to_bare(eo);   // 同第一遍: 载荷/属性/uuid 一起还原
                    if (e != NULL) {
                        for (size_t_ k = 0; k < ac && e != NULL; k++)
                            if (edge_end_add(e, ra[k], 0) != 0) { map_node_free(e); e = NULL; }
                        for (size_t_ k = 0; k < bc && e != NULL; k++)
                            if (edge_end_add(e, rb[k], 1) != 0) { map_node_free(e); e = NULL; }
                    }
                    if (e != NULL && eu != NULL) {
                        char* su = strdup(eu);
                        if (su) { free(e->uuid); e->uuid = su; }
                        refresh_uuid(e);
                        ereg[edge_cnt2].uuid = strdup(eu);
                        ereg[edge_cnt2].nd = e;
                        edge_cnt2++;
                        progress = 1;
                    }
                    free((void*)ra); free((void*)rb);
                }
            }
            // 载入后确定性重算 uuid(注册表释放前): 覆盖存档里的旧值/旧算法值(含不可达节点)。
            // 指纹含父引用 → 迭代到收敛(树状图 1~2 轮; 环状分量上限 8 轮)
            for (int iter = 0; iter < 8; iter++) {
                int ch = 0;
                for (size_t k = 0; k < cnt; k++) if (reg[k].nd != NULL) ch |= rehash_node(reg[k].nd);
                for (size_t k = 0; k < edge_cnt2; k++) if (ereg[k].nd != NULL) ch |= rehash_node(ereg[k].nd);
                if (!ch) break;
            }
            // uuid 去重(同父同内容的历史重复节点): 打 dedup 标记直到无重复
            {
                uuid_rec* rec = (uuid_rec*)malloc((cnt + edge_cnt2 + 1) * sizeof(uuid_rec));
                if (rec != NULL) {
                    for (int round = 0; round < 16; round++) {
                        size_t m = 0;
                        for (size_t k = 0; k < cnt; k++)
                            if (reg[k].nd != NULL && reg[k].nd->uuid != NULL) { rec[m].u = reg[k].nd->uuid; rec[m].nd = reg[k].nd; m++; }
                        for (size_t k = 0; k < edge_cnt2; k++)
                            if (ereg[k].nd != NULL && ereg[k].nd->uuid != NULL) { rec[m].u = ereg[k].nd->uuid; rec[m].nd = ereg[k].nd; m++; }
                        if (load_dedup(rec, m) == 0) break;
                    }
                    free((void*)rec);
                }
            }
            for (size_t k = 0; k < edge_cnt2; k++) free((void*)ereg[k].uuid);
            free(ereg);
        }
    }
    free(reg);
    cJSON_Delete(root);

    if (r == NULL) return -1;
    m->root = r;
    // 载入后按当前指纹算法自顶向下重算 uuid(确定性):
    //   1) 旧存档的 uuid 可能由旧算法产生 —— 跨父碰撞会让索引丢节点
    //   2) 重算保证"同图同 uuid", 与操作历史无关
    refresh_uuid(r);
    return 0;
}

// 保存 .map(明文 JSON; gzip 暂不支持, 后续加)
int map_save(map* m, const char* path) {
    if (m == NULL || m->root == NULL || path == NULL) return -1;
    cJSON* root = node_to_json(m->root);
    // 顶层 edges 区段: 收集全部边节点(遍历去重)
    node_bag bag = {0};
    collect_dfs(m->root, &bag);
    cJSON* je = cJSON_AddArrayToObject(root, "edges");
    for (size_t i = 0; i < bag.count; i++) {
        node* e = bag.items[i];
        if (!node_is_edge(e)) continue;
        cJSON* eo = node_to_json(e);
        cJSON_AddItemToArray(je, eo);
    }
    bag_clear(&bag);
    char* js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (js == NULL) return -1;
    // 原子写: 先写 <path>.tmp, 成功后再替换 —— 防进程中断留下半文件
    size_t pl = strlen(path);
    char* tmp = (char*)malloc(pl + 5);
    if (tmp == NULL) { free(js); return -1; }
    snprintf(tmp, pl + 5, "%s.tmp", path);   // 单次拼接, 自动 null 终止
    FILE* f = fopen(tmp, "wb");
    int rc = -1;
    if (f) {
        rc = (fwrite(js, 1, strlen(js), f) == strlen(js)) ? 0 : -1;
        fclose(f);
        if (rc == 0) {
            remove(path);                 // 覆盖旧档(忽略不存在)
            rc = rename(tmp, path);       // 原子替换(tmp → 正式名)
        }
    }
    free(tmp);
    free(js);
    return rc == 0 ? 0 : -1;
}

// 向已有实例载入档(path), 替换根并释放旧树。失败保持原图不变。
int map_load(map* m, const char* path) {
    if (m == NULL || path == NULL || path[0] == '\0') return -1;
    node* old = m->root;
    if (map_load_file(m, path) != 0) return -1;   // 失败: m->root 未动
    if (old != NULL && old != m->root) map_node_free(old);
    return 0;
}

/* ================= 版本 ================= */
/* 版本号来自 VERSION 文件 → tools/genversion.mjs → version.h(编译前自动生成) */
const char* map_version(void)   { return CATSENA_VERSION; }
const char* map_build(void)     { return CATSENA_BUILD; }
const char* map_buildinfo(void) { return CATSENA_BUILDINFO; }
int         map_abi(void)       { return CATSENA_ABI; }

/* ================= 图实例 ================= */

// 图实例: 载入/保存/锁/视图/索引
map* map_open(const char* path, const MapConfig* config) {
    map* m = (map*)calloc(1, sizeof(map));
    if (m == NULL) return NULL;
    if (config) m->config = *config;
    else m->config.enable_index = 1;   // 未显式配置 → 默认开 uuid 索引(纯内部加速, 无外部语义)
    // 图级读写锁(SRWLock; 单线程模式也建, 无妨)
    map_rwlock_t* sl = (map_rwlock_t*)calloc(1, sizeof(map_rwlock_t));
    if (sl == NULL) { free(m); return NULL; }
    map_rwlock_init(sl);
    m->lock = sl;
    // 根节点: 项目容器(data=图名)
    node* root = map_node_new(DATA_TEXT, NULL, 0);
    if (root == NULL) { free(sl); free(m); return NULL; }
    m->root = root;
    if (path && path[0] && map_load_file(m, path) == 0) {
        // 成功载入, root 已替换
    } else {
        const char* base = path ? strrchr(path, '/') : NULL;
        if (!base) base = path ? strrchr(path, '\\') : NULL;
        const char* nm = base ? base + 1 : (path ? path : "map");
        char* nmc = strdup(nm);
        if (nmc) node_adopt_data(m->root, nmc, strlen(nmc));   // 图名进指纹(同步 sha256 + 重算 uuid)
    }
    if (m->config.enable_index) {
        m->index = idx_new();
        if (m->index == NULL) { map_close(m); return NULL; }
    }
    return m;
}

// 关闭图实例: 释放所有节点/索引/锁, 不保存
void map_close(map* m) {
    if (m == NULL) return;
    if (m->index) idx_free((uuid_index*)m->index);
    if (m->lock) free(m->lock);
    map_node_free(m->root);
    free(m);
}

// 获取根节点(整图解析路径的锚点)
node* map_root(map* m) { return m ? m->root : NULL; }

/* 视图锚定根: 命令路径从此节点解析(应用端用它表达授权根语义) */
int map_set_view(map* m, node* view_root) {
    if (m == NULL) return -1;
    map_lock_write(m);
    m->view_root = view_root;
    map_unlock_write(m);
    return 0;
}

// 获取视图锚定根(命令路径解析的起点, NULL=整图)
node* map_view(map* m) { return m ? m->view_root : NULL; }

// 命令路径解析: 支持 "uuid:<64hex>" 直接寻址(页面定义节点名字=定义文本, 无法用名字路径),
//   以及 "uuid:<64hex>/<子路径>" 混合寻址(先按 uuid 定位, 再沿剩余路径走)。
static node* map_resolve(map* m, node* anchor, const char* spec) {
    if (spec == NULL) return NULL;
    if (strncmp(spec, "uuid:", 5) == 0) {
        const char* rest = spec + 5;
        const char* slash = strchr(rest, '/');
        char uuid_buf[65];
        node* n;
        if (slash == NULL) {
            n = map_find_uuid(m, rest);
        } else {
            size_t len = (size_t)(slash - rest);
            if (len >= sizeof(uuid_buf)) len = sizeof(uuid_buf) - 1;
            memcpy(uuid_buf, rest, len);
            uuid_buf[len] = 0;
            n = map_find_uuid(m, uuid_buf);
            if (n != NULL) n = find_node_by_path(n, slash + 1);
        }
        return n;
    }
    return find_node_by_path(anchor, spec);
}

/* ---- 会话视图(每连接一个; 共享同一 map 实例) ---- */

struct map_session {
    map*  m;
    node* view_root;   /* 会话锚定根(NULL=整图) */
};

// 会话视图: 解析命令路径的锚点, 可由应用端表达授权根语义
map_session* map_session_open(map* m) {
    if (m == NULL) return NULL;
    map_session* s = (map_session*)calloc(1, sizeof(map_session));
    if (s != NULL) s->m = m;
    return s;
}

// 关闭会话视图: 释放会话结构, 不释放 map 实例
void map_session_close(map_session* s) { free(s); }

// 设置会话视图锚定根: 解析命令路径的起点, NULL=整图
int map_session_set_view(map_session* s, const char* path) {
    if (s == NULL) return -1;
    if (path == NULL || path[0] == '\0') { s->view_root = NULL; return 0; }
    node* n = find_node_by_path(s->m->root, path);
    if (n == NULL) return 1;
    s->view_root = n;
    return 0;
}

/* ---- 锁(图级 SRWLock: 读共享/写排他; 释放必须匹配获取类型) ---- */

// 锁定读共享
int map_lock_read(map* m) {
    if (m == NULL || m->lock == NULL) return 0;
    map_rwlock_rd((map_rwlock_t*)m->lock);
    return 0;
}

// 锁定写排他
int map_lock_write(map* m) {
    if (m == NULL || m->lock == NULL) return 0;
    map_rwlock_wr((map_rwlock_t*)m->lock);
    return 0;
}

// 释放读共享
int map_unlock_read(map* m) {
    if (m == NULL || m->lock == NULL) return 0;
    map_rwlock_rdun((map_rwlock_t*)m->lock);
    return 0;
}

// 释放写排他
int map_unlock_write(map* m) {
    if (m == NULL || m->lock == NULL) return 0;
    map_rwlock_wrun((map_rwlock_t*)m->lock);
    return 0;
}

/* ---- 查询 ---- */

// 按 uuid 查找节点(索引优先; 无索引时深搜兜底)
node* map_find_uuid(map* m, const char* uuid) {
    if (m == NULL || uuid == NULL) return NULL;
    if (m->index) {
        uuid_index* ix = (uuid_index*)m->index;
        // 稳态(dirty=0): 共享锁并发查找; 脏: 升级排他重建后再查(双检, 避免重复重建)
        map_rwlock_rd(&ix->lock);
        if (!ix->dirty) {
            node* hit = idx_get(ix, uuid);
            map_rwlock_rdun(&ix->lock);
            return hit;
        }
        map_rwlock_rdun(&ix->lock);
        map_rwlock_wr(&ix->lock);
        if (ix->dirty) idx_rebuild(m, ix);
        node* hit = idx_get(ix, uuid);
        map_rwlock_wrun(&ix->lock);
        return hit;             // 索引权威: 未命中即不存在(不再走 O(N²) 兜底深搜)
    }
    // 无索引: 深搜兜底(哈希集去重, 无节点数上限)
    node_set seen;
    memset(&seen, 0, sizeof seen);
    node* hit = find_uuid_dfs(m->root, uuid, &seen);
    set_free(&seen);
    return hit;
}

// 按路径查找节点(支持 uuid:<64hex> 混合寻址)
node* map_find_path(map* m, const char* path) {
    if (m == NULL || m->root == NULL) return NULL;
    // 感知 map 级锚定根(会话级由 map_session_set_view + 命令层处理)
    return find_node_by_path(m->view_root ? m->view_root : m->root, path);
}

// 收集子树节点(深搜, 指针去重, 不含 NULL, 返回实际收集数)
size_t map_collect_subtree(map* m, node* root, node** arr, size_t cap) {
    (void)m;
    if (root == NULL || arr == NULL) return 0;
    node_bag bag = {0};
    collect_dfs(root, &bag);
    size_t n = bag.count < cap ? bag.count : cap;
    for (size_t i = 0; i < n; i++) arr[i] = bag.items[i];
    bag_clear(&bag);
    return n;
}

// 垃圾回收
int map_gc(map* m) { (void)m; return 0; }

// 事件通知: 图操作后回调(应用端可注册, 例如刷新索引/更新 UI)
int map_emit(map* m, map_evt_kind evt, node* arg) {
    if (m == NULL) return 0;
    if (m->config.hooks && m->config.hooks->on_change)
        return m->config.hooks->on_change(m, evt, arg, m->config.hooks->ud);
    return 0;
}

/* ================= 命令层(图操作, 无业务语义) ================= */

// 切首词(支持引号)
static const char* next_arg(const char* s, char* out, size_t outsz) {
    if (outsz) out[0] = 0;
    if (!s) return NULL;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return s;
    size_t i = 0;
    if (*s == '"') {
        s++;
        while (*s && *s != '"' && i + 1 < outsz) out[i++] = *s++;
        if (*s == '"') s++;
    } else {
        while (*s && *s != ' ' && *s != '\t' && i + 1 < outsz) out[i++] = *s++;
    }
    out[i] = 0;
    return s;
}

// 去首尾空白
static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t l = strlen(s);
    while (l && (s[l-1] == ' ' || s[l-1] == '\t' || s[l-1] == '\n' || s[l-1] == '\r')) s[--l] = 0;
    return s;
}

// 递归 ls 子树(深度缩进, 输出缓冲区)
static void map_ls(node* n, int depth, char* out, size_t outsz, size_t* used) {
    char line[512];
    int len = snprintf(line, sizeof(line), "%*s%s%s\n", depth * 2, "", node_is_edge(n) ? "[E]" : "[T]",
                       n->data ? (const char*)n->data : (n->uuid ? n->uuid : "?"));
    if (*used + (size_t)len < outsz) { memcpy(out + *used, line, (size_t)len); *used += (size_t)len; }
    for (size_t_ i = 0; i < n->childs.count; i++)
        map_ls((node*)n->childs.items[i], depth + 1, out, outsz, used);
}

// 执行命令(内部)
static int map_exec_inner(map* m, node* anchor, const char* cmd, char* out, int outsz) {
    if (m == NULL || anchor == NULL || cmd == NULL || out == NULL || outsz <= 0) return -1;
    out[0] = 0;
    char arg[512], arg2[512], arg3[1024];
    char* line = strdup(cmd);
    if (line == NULL) return -1;
    const char* rest = trim(line);
    rest = next_arg(rest, arg, sizeof(arg));

    // 命令读写分类: 读命令共享锁(graphjson/ls/help/attr 读模式), 其余排他写锁
    int is_read = (strcmp(arg, "ls") == 0 || strcmp(arg, "graphjson") == 0 ||
                   strcmp(arg, "graph") == 0 || strcmp(arg, "get") == 0 ||
                   strcmp(arg, "path") == 0 ||
                   strcmp(arg, "help") == 0 || strcmp(arg, "?") == 0);
    if (!is_read && strcmp(arg, "attr") == 0) {
        const char* r2 = next_arg(rest, arg2, sizeof(arg2));
        r2 = next_arg(r2, arg3, sizeof(arg3));
        is_read = (trim((char*)r2)[0] == '\0');   // 无 value = 读模式
    }
    if (is_read) map_lock_read(m);
    else         map_lock_write(m);

    if (strcmp(arg, "create") == 0) {
        rest = next_arg(rest, arg2, sizeof(arg2));   // parent
        next_arg(rest, arg3, sizeof(arg3));          // name(rest 不再使用)
        if (arg3[0] == 0) { snprintf(out, outsz, "usage: create <parent> <name>\n"); goto done; }
        node* p = map_resolve(m, anchor, arg2);
        if (p == NULL) { snprintf(out, outsz, "create: 找不到 parent '%s'\n", arg2); goto done; }
        // 同父重名检查: 内容寻址下, 同父+同 data+同结构 = 指纹必然相同 → uuid 冲突。
        // 图内路径应唯一: 父下已有同名子节点则拒绝(用 set 改名/更新)。
        if (find_child_by_data(p, arg3) != NULL) {
            snprintf(out, outsz, "create: '%s' 下已存在同名节点 '%s' (图内路径唯一)\n", arg2, arg3);
            goto done;
        }
        char* name_dup = strdup(arg3);
        if (name_dup == NULL) { snprintf(out, outsz, "create: 内存不足\n"); goto done; }
        node* nd = map_node_new(DATA_TEXT, name_dup, 0);
        if (nd == NULL) { free(name_dup); snprintf(out, outsz, "create: 内存不足\n"); goto done; }
        nd->owns_data = 1;
        if (link_parent_child(p, nd) != 0) { map_node_free(nd); snprintf(out, outsz, "create: 链接失败\n"); goto done; }
        snprintf(out, outsz, "create: 已建 '%s' (uuid=%.8s)\n", arg3, nd->uuid ? nd->uuid : "?");
    } else if (strcmp(arg, "set") == 0) {
        rest = next_arg(rest, arg2, sizeof(arg2));   // path
        const char* val = trim((char*)rest);          // 剩余整行为新值(支持多词/空格)
        node* n = map_resolve(m, anchor, arg2);
        if (n == NULL || val[0] == 0) { snprintf(out, outsz, "usage: set <path> <newdata>\n"); goto done; }
        // 改名撞名检查: 同父兄弟已有同名 → 图内路径冲突(否则同构 → uuid 相同)
        for (size_t_ pi = 0; pi < n->parents.count; pi++) {
            node* p = (node*)n->parents.items[pi];
            for (size_t_ ci = 0; ci < p->childs.count; ci++) {
                node* sib = (node*)p->childs.items[ci];
                if (sib != n && sib->data && sib->data_len == strlen(val) &&
                    memcmp(sib->data, val, sib->data_len) == 0) {
                    snprintf(out, outsz, "set: 改名冲突 — 同父下已有同名节点 (图内路径唯一)\n");
                    goto done;
                }
            }
        }
        char* nd = strdup(val);
        if (nd == NULL) goto done;
        if (n->owns_data) free((void*)n->data);
        n->data = nd;
        n->owns_data = 1;
        n->data_len = strlen(nd);
        free(n->sha256);
        {
            char* hex = (char*)malloc(65);
            if (hex) {
                sha256(nd, n->data_len, hex);   // 直接填充 hex，返回 0 表示成功
            }
            n->sha256 = hex;
        }
        free(n->uuid);
        n->uuid = node_compute_uuid(n);
        snprintf(out, outsz, "set: 已更新 '%s'\n", arg2);
    } else if (strcmp(arg, "attr") == 0) {
        rest = next_arg(rest, arg2, sizeof(arg2));
        rest = next_arg(rest, arg3, sizeof(arg3));
        if (arg2[0] == 0 || arg3[0] == 0) { snprintf(out, outsz, "usage: attr <path> <key> [value|-]\n"); goto done; }
        node* n = map_resolve(m, anchor, arg2);
        if (n == NULL) { snprintf(out, outsz, "attr: 找不到 '%s'\n", arg2); goto done; }
        const char* raw = trim((char*)rest);
        // 剥一层包裹引号: `attr a/b view "gallery"` → gallery
        //   (命令行里值带空格时只能加引号; 引号本身不是值的一部分)
        char valbuf[1024];
        const char* val = raw;
        size_t rl = strlen(raw);
        if (rl >= 2 && ((raw[0] == '"' && raw[rl - 1] == '"') || (raw[0] == '\'' && raw[rl - 1] == '\''))) {
            size_t inner = rl - 2;
            if (inner >= sizeof(valbuf)) inner = sizeof(valbuf) - 1;
            memcpy(valbuf, raw + 1, inner);
            valbuf[inner] = 0;
            val = valbuf;
        }
        if (val[0] == 0) {
            const char* v = node_attr_get(n, arg3);
            snprintf(out, outsz, "attr: %s.%s = %s\n", arg2, arg3, v ? v : "(none)");
        } else if (strcmp(val, "-") == 0) {
            int rc = node_attr_set(n, arg3, NULL);
            snprintf(out, outsz, "attr: 已删除 %s.%s rc=%d\n", arg2, arg3, rc);
        } else {
            int rc = node_attr_set(n, arg3, val);
            snprintf(out, outsz, "attr: %s.%s = %s rc=%d\n", arg2, arg3, val, rc);
        }
    } else if (strcmp(arg, "del") == 0 || strcmp(arg, "rm") == 0) {
        rest = next_arg(rest, arg2, sizeof(arg2));
        if (arg2[0] == 0) { snprintf(out, outsz, "usage: del <path>\n"); goto done; }
        node* n = map_resolve(m, anchor, arg2);
        if (n == NULL) { snprintf(out, outsz, "del: 找不到 '%s'\n", arg2); goto done; }
        if (n == anchor) { snprintf(out, outsz, "del: 不能删除锚定根\n"); goto done; }
        // 从所有父摘除后释放
        while (n->parents.count > 0) {
            node* p = (node*)n->parents.items[0];
            ptrdiff_t i = arr_find(&p->childs, n);
            if (i >= 0) { for (size_t_ j = (size_t_)i; j < p->childs.count - 1; j++) p->childs.items[j] = p->childs.items[j+1]; p->childs.count--; }
            for (size_t_ j = 0; j < n->parents.count - 1; j++) n->parents.items[j] = n->parents.items[j+1];
            n->parents.count--;
            refresh_uuid(p);   // 父节点 childs 变化 → 指纹重算
        }
        map_node_free(n);
        snprintf(out, outsz, "del: 已删除\n");
    } else if (strcmp(arg, "neighbor") == 0 || strcmp(arg, "link") == 0) {
        rest = next_arg(rest, arg2, sizeof(arg2));
        rest = next_arg(rest, arg3, sizeof(arg3));
        node* a = map_resolve(m, anchor, arg2);
        node* b = map_resolve(m, anchor, arg3);
        if (!a || !b) { snprintf(out, outsz, "neighbor: 找不到节点\n"); goto done; }
        int rc = link_neighbor(a, b);
        snprintf(out, outsz, "neighbor: rc=%d\n", rc);
    } else if (strcmp(arg, "edge") == 0) {
        rest = next_arg(rest, arg2, sizeof(arg2));
        rest = next_arg(rest, arg3, sizeof(arg3));   // 赋回: 后续需取内容参数
        node* a = map_resolve(m, anchor, arg2);
        node* b = map_resolve(m, anchor, arg3);
        if (!a || !b) { snprintf(out, outsz, "edge: 找不到节点\n"); goto done; }
        const char* content = trim((char*)rest);   // 第三参=边载荷(可空; 空边=邻节点等价, 带内容=跨级中转)
        node* e = (content[0] != '\0')
            ? edge_connect_with(a, b, strdup(content), strlen(content))
            : edge_connect(a, b);
        if (e != NULL && content[0] != '\0') e->owns_data = 1;   // strdup 自持
        snprintf(out, outsz, "edge: %s (uuid=%.8s)\n", e ? "已建" : "失败", e && e->uuid ? e->uuid : "?");
    } else if (strcmp(arg, "elink") == 0) {
        /* elink <边> <节点> <a|b> [+|-]  —— 往边的一端加减成员(幂等)
           一条边两端都是集合: 普通关系各 1 个, 收纳/分组 = A 端 1 个, 共享边 = 两端多个。
           摘到某端空了 → 这条边不再是边, 直接消失。 */
        rest = next_arg(rest, arg2, sizeof(arg2));   // 边
        rest = next_arg(rest, arg3, sizeof(arg3));   // 节点
        char sidebuf[8] = {0}, opbuf[8] = {0};
        rest = next_arg(rest, sidebuf, sizeof(sidebuf));
        next_arg(rest, opbuf, sizeof(opbuf));
        node* e = map_resolve(m, anchor, arg2);
        node* n = map_resolve(m, anchor, arg3);
        int side = (sidebuf[0] == 'b' || sidebuf[0] == 'B') ? 1 : 0;
        int off = (opbuf[0] == '-');
        if (!e || !n) { snprintf(out, outsz, "elink: 找不到节点\n"); goto done; }
        char nb[24]; brief_name(n, nb, sizeof nb);
        if (!off) {
            if (edge_end_add(e, n, side) != 0) { snprintf(out, outsz, "elink: 失败\n"); goto done; }
            snprintf(out, outsz, "elink: %s端 +%s → A=%d B=%d\n",
                     side ? "B" : "A", nb,
                     (int)e->edge_as.count, (int)e->edge_bs.count);
        } else {
            if (edge_end_del(e, n, side) != 0) { snprintf(out, outsz, "elink: 该节点不在这端\n"); goto done; }
            if (!node_is_edge(e)) {
                map_node_free(e);   // 某一端空了 → 关系不成立, 整条边消失
                snprintf(out, outsz, "elink: 端点已摘空 → 边已删除\n");
            } else {
                snprintf(out, outsz, "elink: %s端 -%s → A=%d B=%d\n",
                         side ? "B" : "A", nb,
                         (int)e->edge_as.count, (int)e->edge_bs.count);
            }
        }
    } else if (strcmp(arg, "ls") == 0) {
        rest = next_arg(rest, arg2, sizeof(arg2));
        node* n = arg2[0] ? map_resolve(m, anchor, arg2) : anchor;
        if (n == NULL) { snprintf(out, outsz, "ls: 找不到 '%s'\n", arg2); goto done; }
        size_t used = 0;
        map_ls(n, 0, out, outsz, &used);
    } else if (strcmp(arg, "get") == 0) {
        rest = next_arg(rest, arg2, sizeof(arg2));
        node* n = arg2[0] ? map_resolve(m, anchor, arg2) : NULL;
        if (n == NULL) { snprintf(out, outsz, "get: 找不到 '%s'\n", arg2); goto done; }
        // 输出 data 原文(应用端语义层用它读页面定义等)
        if (n->data != NULL && n->data_len > 0)
            snprintf(out, outsz, "%s", (const char*)n->data);
        else
            snprintf(out, outsz, "(no data)");
    } else if (strcmp(arg, "path") == 0) {
        rest = next_arg(rest, arg2, sizeof(arg2));
        node* n = arg2[0] ? map_resolve(m, anchor, arg2) : anchor;
        // 输出节点 uuid(应用端语义层定位用; "(none)" 表示不存在)
        if (n != NULL && n->uuid != NULL) snprintf(out, outsz, "%s\n", n->uuid);
        else snprintf(out, outsz, "(none)\n");
    } else if (strcmp(arg, "graphjson") == 0 || strcmp(arg, "graph") == 0) {
        // 与旧内核同构: {root, nodes:[{id,dtype,uuid,sys,name,attrs,...}], links:[{a,b,kind}]}
        // 从锚定根(会话 view_root)输出 —— 授权根语义由应用端设置
        node_bag bag = {0};
        collect_dfs(anchor, &bag);
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "root", anchor->uuid ? anchor->uuid : "");
        cJSON* na = cJSON_AddArrayToObject(o, "nodes");
        for (size_t i = 0; i < bag.count; i++) {
            node* nd = bag.items[i];
            cJSON* no = cJSON_CreateObject();
            cJSON_AddNumberToObject(no, "id", (double)nd->id);
            cJSON_AddNumberToObject(no, "dtype", (double)(int)nd->dtype);
            if (nd->uuid) cJSON_AddStringToObject(no, "uuid", nd->uuid);
            // 边角色标记(内容判定式: 两端集合都非空) —— 前端据此渲染类型, 不再看 dtype
            if (node_is_edge(nd)) {
                cJSON_AddNumberToObject(no, "edge", 1);
                /* 头尾端点(A=头 / B=尾) —— 前端据此画方向箭头。
                   单端给旧的 edge?_uuid(老前端不动), 多端额外给数组。 */
                const node* a0 = (const node*)nd->edge_as.items[0];
                const node* b0 = (const node*)nd->edge_bs.items[0];
                if (a0 && a0->uuid) cJSON_AddStringToObject(no, "edge_a_uuid", a0->uuid);
                if (b0 && b0->uuid) cJSON_AddStringToObject(no, "edge_b_uuid", b0->uuid);
                if (nd->edge_as.count > 1) add_uuid_arr(no, "edge_a_uuids", &nd->edge_as);
                if (nd->edge_bs.count > 1) add_uuid_arr(no, "edge_b_uuids", &nd->edge_bs);
            }
            // 兼容层: attrs.hidden=1 → sys:1(应用端约定, 前端据此过滤渲染)
            if (node_attr_get(nd, "hidden") != NULL) cJSON_AddNumberToObject(no, "sys", 1);
            if ((nd->dtype == DATA_TEXT || nd->dtype == DATA_FILE) && nd->data && nd->data_len > 0)
                cJSON_AddStringToObject(no, "name", (const char*)nd->data);
            if (nd->attrs.count > 0) {
                cJSON* ja = cJSON_AddObjectToObject(no, "attrs");
                for (size_t_ k = 0; k < nd->attrs.count; k++) {
                    node_attr* a = (node_attr*)nd->attrs.items[k];
                    if (a && a->key && a->key[0]) cJSON_AddStringToObject(ja, a->key, a->value ? a->value : "");
                }
            }
            cJSON_AddItemToArray(na, no);
        }
        cJSON* la = cJSON_AddArrayToObject(o, "links");
        for (size_t i = 0; i < bag.count; i++) {
            node* nd = bag.items[i];
            for (size_t_ k = 0; k < nd->childs.count; k++) {
                node* c = (node*)nd->childs.items[k];
                if (c && nd->uuid && c->uuid) {
                    cJSON* lo = cJSON_CreateObject();
                    cJSON_AddStringToObject(lo, "a", nd->uuid);
                    cJSON_AddStringToObject(lo, "b", c->uuid);
                    cJSON_AddStringToObject(lo, "kind", "child");
                    cJSON_AddItemToArray(la, lo);
                }
            }
            for (size_t_ k = 0; k < nd->neighbors.count; k++) {
                node* nb = (node*)nd->neighbors.items[k];
                if (nb && nd->uuid && nb->uuid && nd->id < nb->id) {   // 无向只出一半
                    cJSON* lo = cJSON_CreateObject();
                    cJSON_AddStringToObject(lo, "a", nd->uuid);
                    cJSON_AddStringToObject(lo, "b", nb->uuid);
                    cJSON_AddStringToObject(lo, "kind", "neighbor");
                    cJSON_AddItemToArray(la, lo);
                }
            }
            for (size_t_ k = 0; k < nd->edges.count; k++) {
                node* e = (node*)nd->edges.items[k];
                if (e == NULL || !node_is_edge(e)) continue;
                /* 只从"第一个 A 端"出发输出一次(否则两端都会各输出一遍) */
                if (e->edge_as.items[0] != nd) continue;
                /* 一条边现在可能两端都是集合 → 输出**所有配对**(a_i, b_j),
                   每对都带 edge 字段指回边节点。旧的只读 a/b 的消费方照样能用。 */
                for (size_t_ ai = 0; ai < e->edge_as.count; ai++) {
                    node* ea = (node*)e->edge_as.items[ai];
                    if (ea == NULL || ea->uuid == NULL) continue;
                    for (size_t_ bi = 0; bi < e->edge_bs.count; bi++) {
                        node* eb = (node*)e->edge_bs.items[bi];
                        if (eb == NULL || eb->uuid == NULL) continue;
                        cJSON* lo = cJSON_CreateObject();
                        cJSON_AddStringToObject(lo, "a", ea->uuid);
                        cJSON_AddStringToObject(lo, "b", eb->uuid);
                        cJSON_AddStringToObject(lo, "kind", "edge");
                        if (e->uuid) cJSON_AddStringToObject(lo, "edge", e->uuid);
                        cJSON_AddItemToArray(la, lo);
                    }
                }
            }
        }
        bag_clear(&bag);
        char* js = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        if (js) { snprintf(out, outsz, "%s", js); free(js); }
    } else if (strcmp(arg, "save") == 0) {
        const char* p = trim((char*)rest);
        int rc = map_save(m, p[0] ? p : "map.map");
        snprintf(out, outsz, "save: rc=%d\n", rc);
    } else if (strcmp(arg, "help") == 0 || strcmp(arg, "?") == 0) {
        snprintf(out, outsz,
            "catsena 命令:\n"
            "  create <parent> <name>   建 TEXT 子节点(\"\" = 根)\n"
            "  set <path> <newdata>     改节点 data\n"
            "  attr <path> <key> [v|-]  属性读写(应用端语义)\n"
            "  get <path>               读节点 data 原文(uuid: 前缀可寻址)\n"
            "  del <path>               删除节点\n"
            "  neighbor <a> <b>         无向邻接\n"
            "  edge <a> <b>             边节点\n"
            "  elink <边> <节点> <a|b> [+|-]  给边加减端点(1→N / 共享边)\n"
            "  ls [path]                打印子树\n"
            "  graphjson                图结构导出(前端用)\n"
            "  save [path]              存档(.map)\n");
    } else {
        snprintf(out, outsz, "未知命令: %s (help)\n", arg);
    }

done:
    // 只有写命令会改图(结构/uuid/data) → 索引失效; 读命令不标脏(否则每次读都全量重建)
    if (m->index && !is_read) ((uuid_index*)m->index)->dirty = 1;
    map_emit(m, MAP_EVT_SAVE, NULL);
    free(line);
    if (is_read) map_unlock_read(m);
    else         map_unlock_write(m);
    int n = (int)strlen(out);
    return n > outsz - 1 ? outsz - 1 : n;
}

/* map 级执行: 锚定根 = map.view_root 或整图 root */
int map_exec(map* m, const char* cmd, char* out, int outsz) {
    if (m == NULL) return -1;
    return map_exec_inner(m, (m->view_root ? m->view_root : m->root), cmd, out, outsz);
}

/* 会话级执行: 锚定根 = 会话 view_root(每连接独立) */
int map_session_exec(map_session* s, const char* cmd, char* out, int outsz) {
    if (s == NULL || s->m == NULL) return -1;
    return map_exec_inner(s->m, (s->view_root ? s->view_root : s->m->root), cmd, out, outsz);
}

// 异步提交命令: 由 engine 线程池执行, 回调 done(ud, rc) 通知完成
int map_submit(map* m, const char* cmd, char* out, int outsz,
               void (*done)(void* ud, int rc), void* ud) {
    if (m == NULL || cmd == NULL) return -1;
    map_cmd* c = (map_cmd*)calloc(1, sizeof(map_cmd));
    if (c == NULL) return -1;
    c->text = strdup(cmd);
    c->out = out; c->outsz = outsz; c->done = done; c->ud = ud;
    if (c->text == NULL) { free(c); return -1; }
    if (m->engine != NULL)
        return engine_submit(m->engine, m, c);
    int rc = map_exec(m, cmd, out, outsz);
    if (done) done(ud, rc);
    free(c->text);
    free(c);
    return rc;
}
