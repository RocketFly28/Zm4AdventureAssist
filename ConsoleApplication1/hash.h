#ifndef MEMO_HASH_H
#define MEMO_HASH_H

#include "common.h"
#include <stdint.h>
#include <stddef.h>   /* size_t */
#include <stdlib.h>   /* calloc / free */

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 开放寻址哈希表 =====================
 * 最高位（bit63）作为"该槽位已被占用"的标记位。
 * 约定：写入表里的 key 一律是 (用户 key | KEY_IS_USED)；
 *       读取/落盘时用 (key & ~KEY_IS_USED) 还原用户 key。
 * ======================================================== */
#define KEY_IS_USED        0x8000000000000000ULL

typedef struct {
    uint64_t key;
    double   val;       /* dp 子问题最优值 */
} HashEntry;

typedef struct {
    HashEntry *table;
    size_t     capacity;  /* 2 的幂 */
    size_t     max_capacity;
    size_t     used_cnt;
} MemoHash;

/*
 * 初始化开放寻址哈希表（memo 缓存）。
 * 容量自动向上取整为 2 的幂；表项全部清零。
 * 参数：h - 哈希表指针；init_size - 初始容量建议值
 */
int memo_hash_init(MemoHash *h, size_t init_size);

/*
 * 安全地初始化开放寻址哈希表（memo 缓存）。
 * 容量自动向上取整为 2 的幂；表项全部清零。
 * 参数：h - 哈希表指针；init_size - 初始容量建议值； max_size - 容量最大值
 */
int memo_hash_init_s(MemoHash *h, size_t init_size, size_t max_size);

/*
 * 将哈希表压缩，蒸馏主要成分。
 * 压缩失败则不做处理，保留原表。
 * 参数：h - 哈希表指针
 */
void memo_hash_compress(MemoHash *h);

/*
 * 将哈希表容量翻倍（扩容），并把旧表有效项重新哈希迁入新表。
 * 扩容失败时保留旧表继续工作，不置空、不泄漏。
 * 参数：h - 哈希表指针
 */
void memo_hash_resize(MemoHash *h);

/*
 * 在哈希表中查找指定 key。
 * 采用线性探测；遇到空槽即停止，返回 NULL。
 * 参数：h - 哈希表指针；key - 待查找键（用户 key，不含标记位）
 * 返回：命中返回 HashEntry 指针；未命中返回 NULL
 */
HashEntry *memo_hash_lookup(MemoHash *h, uint64_t key);

/*
 * 向哈希表写入键值对；若 key 已存在则覆盖旧值。
 * 装载因子 >= 0.7 时自动触发扩容。
 * 参数：h - 哈希表指针；key - 键；val - DP 子问题最优值
 * need_lookup = false 时，调用方必须保证 key 不在表中，否则后果自负
 */
void memo_hash_put(MemoHash *h, uint64_t key, double val, int need_lookup);

/*
 * 释放哈希表占用的内存，并清空字段。
 * 参数：h - 哈希表指针
 */
void memo_hash_destroy(MemoHash *h);

#ifdef __cplusplus
}
#endif
#endif /* MEMO_HASH_H */
