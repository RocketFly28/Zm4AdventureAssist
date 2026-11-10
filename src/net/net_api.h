#ifndef NET_API_H
#define NET_API_H

#include "../dpsolver/Adventure.h"

/**
 * @brief 使用神经网络闪电评估当前题目下四个选项，输出每个选项的评估结果
 *
 * @param ctx       游戏上下文，的最小状态
 * @param q_idx     当前待作答题目在题库中的索引
 * @param target_T  目标时间/回合参数，用于评估打分
 * @param out[4]    [输出] 长度为 4 的模型预计分数数组，依次对应选项 0～选项 3
 * @return int      返回状态码；0 代表评估成功，非 0 为失败
 *
 * @details
 * 根据 GameContext 内的当前状态、指定题号的题目配置，对四个选择分支做评估，
 * 计算各选项对应的价值、风险、收益等信息，写入 out 数组，供后续策略选择使用。
 *
 * @note
 * - out 由调用方分配，必须保证至少可容纳 4 个 OptionEval；
 * - 内部依赖题库画像、属性归一、门槛判定逻辑，需要与特征构建保持一致；
 * - q_idx 是内核中的合法下标，换算成独热编码时需要注意
 */
int flash_evaluate_options(GameContext *ctx, int q_idx, int target_T, OptionEval out[4]);

#endif // net_api.h