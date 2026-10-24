/* 每个头文件中必须#include common.h */
#ifndef COMMON_H
#define COMMON_H

/* ========================   游戏版本  =============================== */
/* v1: 基础版本，计算最高期望得分 dpMaxAverage()
 * v2: 增加了单局最大化目标得分概率（冲刺高分）功能 dpGoalProb()
 *     新增 cache_load_global 和 cache_save_global保存方式
 *     key增加字段：key[44-51]: r（还需得分）
 * v3: 增加多局冲刺高总分功能 dpGoalProbMax()
 *     dpGoalProb() 重写为调用 dpGoalProbMax()
 *     r（还需得分）→ able_lost_s（还能丢分），语义变更
 *     删除了 cache_load_global 和 cache_save_global
 */
#define GAME_VERSION   3U // 版本号

#ifndef DEBUG
// #define DEBUG
#endif


/* 默认不定义 WASM，需要时编译加 -DWASM 或取消注释 */
#ifndef WASM
#define WASM
#endif

/* 预先计算模式 */
#ifndef PRE_CAL
// #define PRE_CAL
#endif

#ifdef WASM
#undef DEBUG
#define CONSOLE_OFF
#else
#define CONSOLE
#endif

#endif // common.h

