#include "hash.h"

#include <stdio.h>

/*
 * 64 位无符号整数哈希函数（Murmur3 风格混合）。
 * 参数：k - 待哈希的 64 位键
 * 返回：size_t 长度的哈希值，类型适配capacity
 */
static inline size_t hash_u64(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return (size_t)k;
}

/*
 * 返回 >= n 的最小 2 的幂。
 * 参数：n - 目标大小
 * 返回：不小于 n 的最小 2 的幂
 */
static inline size_t next_pow2(size_t n)
{
    size_t v = 1;
    while (v < n) v <<= 1;
    return v;
}

/*
 * 初始化开放寻址哈希表（memo 缓存）。
 * 容量自动向上取整为 2 的幂；表项全部清零。
 * 参数：h - 哈希表指针；init_size - 初始容量建议值
 */
int memo_hash_init(MemoHash *h, size_t init_size)
{
    size_t cap = next_pow2(init_size);
    h->table = (HashEntry *)calloc(cap, sizeof(HashEntry));
    if (!h->table)
    {
        h->capacity = 0;
        h->max_capacity = 0;
        return -1;
    }
    h->capacity = cap;
    h->max_capacity = SIZE_MAX;
    h->used_cnt = 0;
    return 0;
}

/*
 * 安全地初始化开放寻址哈希表（memo 缓存）。
 * 注意：尽量不要使用锁死内存，否则影响计算
 * 容量自动向上取整为 2 的幂；表项全部清零。
 * 参数：h - 哈希表指针；init_size - 初始容量建议值； max_size - 容量最大值
 */
int memo_hash_init_s(MemoHash *h, size_t init_size, size_t max_size)
{
    size_t cap = next_pow2(init_size);
    cap = (cap <= max_size) ? cap : max_size; 
    h->table = (HashEntry *)calloc(cap, sizeof(HashEntry));
    if (!h->table)
    {
        h->capacity = 0;
        h->max_capacity = 0;
        return -1;
    }
    h->capacity = cap;
    h->max_capacity = max_size;
    h->used_cnt = 0;
    return 0;
}

/* 快速降序排序辅助函数 */
static int cmp_hash(const void *a, const void *b)
{
    const HashEntry *ea = (const HashEntry *)a;
    const HashEntry *eb = (const HashEntry *)b;
    int ua = (ea->key & KEY_IS_USED) ? 1 : 0;
    int ub = (eb->key & KEY_IS_USED) ? 1 : 0;

    if (ua != ub) return ub - ua;
    if (ua == 0) return 0;

    // 都是 used，按 val 降序
    if (ea->val < eb->val) return 1;
    if (ea->val > eb->val) return -1;
    return 0;
}

/*
 * 将哈希表压缩，蒸馏主要成分。
 * 压缩失败则不做处理，保留原表。
 * 参数：h - 哈希表指针
 */
void memo_hash_compress(MemoHash *h)
{
    HashEntry *new_table = NULL;
    HashEntry *old_tab = h->table;
    size_t cap;
    for (cap = h->capacity >> 1; cap > 0; cap >>= 1)//从1/2大小开始尝试
    {
        new_table = (HashEntry *)calloc(cap, sizeof(HashEntry));
        if (new_table) break;
    }
    if (!new_table) return; // 如果新表都新建不了，就别动了，凑活跑比崩溃强

    h->table = new_table;
    qsort(old_tab, h->capacity, sizeof(HashEntry), cmp_hash); // 压缩，val比较大的更可能是主路径
    h->capacity = cap;
    h->used_cnt = 0;
    for (size_t i = cap * 0.1; i < cap * 0.4; i++) {
        HashEntry *e = &old_tab[i];
        if (e->key & KEY_IS_USED) {
            size_t hv = hash_u64(e->key & ~KEY_IS_USED);
            size_t idx = hv & (h->capacity - 1U);
            while (h->table[idx].key & KEY_IS_USED)
                idx = (idx + 1U) & (h->capacity - 1U);
            h->table[idx].key = e->key;
            h->table[idx].val = e->val;
            h->used_cnt += 1U;
        }
    }
    free(old_tab);
    old_tab = NULL;
}


/*
 * 将哈希表容量翻倍（扩容），并把旧表有效项重新哈希迁入新表。
 * 扩容失败时压缩。
 * 参数：h - 哈希表指针
 */
void memo_hash_resize(MemoHash *h)
{
    if (h == NULL || h->capacity == 0)
        return;
    size_t old_cap = h->capacity;
    HashEntry *old_tab = h->table;
    size_t new_cap = old_cap << 1U;
    if (new_cap > h->max_capacity)
    {
        memo_hash_compress(h); /* 表太大，压缩 */
        return;
    }

    HashEntry *new_tab = (HashEntry *)calloc(new_cap, sizeof(HashEntry));
    if (new_tab == NULL)
    {
        memo_hash_compress(h);
        return;
    }
           
    h->table = new_tab;
    h->capacity = new_cap;
    h->used_cnt = 0;

    for (size_t i = 0; i < old_cap; i++) {
        HashEntry *e = &old_tab[i];
        if (e->key & KEY_IS_USED) {
            size_t hv = hash_u64(e->key & ~KEY_IS_USED);
            size_t idx = hv & (h->capacity - 1U);
            while (h->table[idx].key & KEY_IS_USED)
                idx = (idx + 1U) & (h->capacity - 1U);
            h->table[idx].key = e->key;
            h->table[idx].val = e->val;
            h->used_cnt += 1U;
        }
    }
    free(old_tab);
}

/*
 * 在哈希表中查找指定 key。
 * 采用线性探测；遇到空槽即停止，返回 NULL。
 * 参数：h - 哈希表指针；key - 待查找键（用户 key，不含标记位）
 * 返回：命中返回 HashEntry 指针；未命中返回 NULL
 */
HashEntry *memo_hash_lookup(MemoHash *h, uint64_t key)
{
    if (key == UINT64_MAX) return NULL;
    size_t hv = hash_u64(key);
    size_t idx = hv & (h->capacity - 1U);
    for (size_t i = 0; i < h->capacity; i++) {
        HashEntry *e = &h->table[idx];
        if (!(e->key & KEY_IS_USED)) return NULL;
        if (e->key & KEY_IS_USED && e->key == (key | KEY_IS_USED)) return e;
        idx = (idx + 1U) & (h->capacity - 1U);
    }
    return NULL;
}

/*
 * 向哈希表写入键值对；若 key 已存在则覆盖旧值。
 * 装载因子 >= 0.7 时自动触发扩容。
 * 参数：h - 哈希表指针；key - 键；val - DP 子问题最优值
 * need_lookup = false 时，调用方必须保证 key 不在表中，否则后果自负
 */
void memo_hash_put(MemoHash *h, uint64_t key, double val, int need_lookup)
{
    if (key == UINT64_MAX) return;

    if (need_lookup)
    {
        HashEntry *e_exist = memo_hash_lookup(h, key);
        if (e_exist != NULL) { e_exist->val = val; return; }
    }

    #ifdef DEBUG
    if (!need_lookup){
        HashEntry *e_chk = memo_hash_lookup(h, key);
        if (e_chk != NULL) {
            fprintf(stderr, "重复 put: key=%016llx\n", (unsigned long long)key);
            fprintf(stderr, "  new_val=%.6f, old_val=%.6f\n", val, e_chk->val);
            fprintf(stderr, "  cap=%zu, used=%zu\n", h->capacity, h->used_cnt);
            // 分解 key
            fprintf(stderr, "  mask=%u h=%u p=%u w=%u c=%u l=%u a=%u done=%u r=%u\n",
                (unsigned)(key & 0xFFFF),
                (unsigned)((key >> 16) & 0xF),
                (unsigned)((key >> 20) & 0xF),
                (unsigned)((key >> 24) & 0xF),
                (unsigned)((key >> 28) & 0xF),
                (unsigned)((key >> 32) & 0xF),
                (unsigned)((key >> 36) & 0xF),
                (unsigned)((key >> 40) & 0xF),
                (unsigned)((key >> 44) & 0xFF));
            abort();
        }
    }
    #endif

    double load = (double)h->used_cnt / (double)h->capacity;
    if (load >= 0.7)
        memo_hash_resize(h);

    size_t hv = hash_u64(key);
    size_t idx = hv & (h->capacity - 1U);
    for (size_t i = 0; i < h->capacity; i++) {
        HashEntry *e = &h->table[idx];
        if (!(e->key & KEY_IS_USED)) {
            e->key = key | KEY_IS_USED;
            e->val = val;
            h->used_cnt += 1U;
            return;
        }
        idx = (idx + 1U) & (h->capacity - 1U);
    }
    /* 扩容后理论不会走到这里；为内核安全，直接放弃本次插入，不再 abort() */
}

/*
 * 释放哈希表占用的内存，并清空字段。
 * 参数：h - 哈希表指针
 */
void memo_hash_destroy(MemoHash *h)
{
    free(h->table);
    h->table = NULL;
    h->capacity = 0;
    h->used_cnt = 0;
}
