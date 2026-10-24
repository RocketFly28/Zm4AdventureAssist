#ifndef DP_CAL_H
#define DP_CAL_H

/* ===========================================================================
 * dpCal：内核 DP 计算模块（Adventure.c 中拆分出来）
 *
 *   - 对外只暴露两个纯计算入口：dpMaxAverage / dpGoalProb。
 *   - 两者都只通过指针参数访问 memo，不碰任何全局变量，因此可重入、
 *     支持同一进程同时跑多局。
 *   - memo 由调用方（GameContext）持有并负责生命周期，本模块只读写。
 * =========================================================================== */

#include "common.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "Adventure.h"
#include "hash.h"


#ifdef __cplusplus
extern "C" {
#endif

/*
 * DP：从 (group_mask, raw_attr, done_cnt) 状态出发的"剩余期望最大得分"。
 * 每个未选题目取 4 个选项中期望最高的一个并求和，再对本组剩余题数取平均。
 * 参数：memo - 期望分 memo 哈希表；group_mask - 本组已选题掩码；
 *       raw_attr - 当前属性；done_cnt - 已完成题数 0~10
 * 返回：剩余期望得分
 */
double dpMaxAverage(MemoHash *memo, uint16_t group_mask, const Attrs raw_attr,
                    int done_cnt);

/*
 * 【薄适配层 · 不再单独维护】
 * 旧入口：计算"单局内使距目标分数分差 r 最小化"的策略与概率。
 * 现在不再独立维护，而是一层转发：把 r 换算成 able_lost_s 后直接调
 * dpGlobalProMax（game_round 取 MAX_ROUND，即从最后一局的角度算单局最优，
 * 无需查表，故 win_rates 传 NULL）。
 * 参数：memo - 概率 memo 哈希表；group_mask - 本组已选题掩码；
 *       raw_attr - 当前属性；done_cnt - 已完成题数 0~10；r - 还差分值
 * 返回：达标概率
 */
double dpGoalProb(MemoHash *memo, uint16_t group_mask, const Attrs raw_attr,
                  int done_cnt, int r);

/*
 * DP：多局全局最优达标概率（按"可丢分预算"递归）。
 * win_rates : 一维展平的概率表，口径为 win_rates[game_round * stride + able_lost_s]。
 *             调用者需保证它指向当前正在算的那张表（win_rates80 或 win_rates220），
 *             且长度 >= MAX_ROUND * stride；传 NULL 时表示最后一轮不需要查表。
 * stride    : 该表的行步长 = 表列数 = max_lose + 1（80 档传 81，220 档传 221）。
 *             展平后必须由调用者提供，否则会整体错位读到错误数值。
 */
double dpGlobalProMax(MemoHash *memo, uint16_t group_mask, const Attrs role_attr,
    int done_cnt, int able_lost_s, int game_round,
    const double *win_rates, int stride);

const double *winrate80;
const double *winrate220;

#ifdef __cplusplus
}
#endif
#endif /* DP_CAL_H */
