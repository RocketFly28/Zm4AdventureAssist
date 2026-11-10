# ifndef NET_H
# define NET_H

/**
 * @brief 神经网络模型前向推理入口，接收扁平化状态特征向量，输出策略与价值
 *
 * @param flat_input  输入扁平特征向量，float 数组，总长度 202
 * @param p_out       输出模型预测的四选项概率 p
 * @param v_out       输出模型预测的分数，可为NULL
 * @return int        输出模型预测的最佳选项序号 0-3，即argmax p
 *
 * @details
 * 特征排布（flat_input 连续布局）：
 *
 * - [0 ~ 40] (41)：q，当前题目 One-hot
 *   如果当前是题库第 3 题，则 flat_input[3] = 1.0，其余为 0.0。
 *
 * - [41 ~ 64] (24)：need，4 个选项的 6 维门槛
 *   顺序：选项 0(hp, pow, wis, cha, luck, agi)，选项 1，选项 2，选项 3。
 *
 * - [65 ~ 92] (28)：succ，4 个选项成功收益
 *   顺序：选项 0(得分, hp, pow, wis, cha, luck, agi)，选项 1，选项 2，选项 3。
 *
 * - [93 ~ 120] (28)：fail，4 个选项失败惩罚
 *   顺序：选项 0(得分, hp, pow, wis, cha, luck, agi)，选项 1，选项 2，选项 3。
 *
 * - [121] (1)：health，当前生命值（需与 Python 归一逻辑对齐）
 *
 * - [122 ~ 126] (5)：other，其余五维属性
 *   顺序：power, wisdom, charm, luck, agility
 *
 * - [127 ~ 130] (4)：round，回合与分数缩放特征
 *   - 127：round_sc         第几题 / 10.0
 *   - 128：score_sc         当前分数 / 100.0
 *   - 129：target_sc        目标分数 / 100.0
 *   - 130：remain_round_sc  (10 - 当前题数) / 10.0
 *
 * - [131] (1)：step，已答题数缩放
 *   值：已答题数 / 10.0
 *
 * - [132 ~ 166] (35)：arch_need，剩余题库需求画像，共 5 类，每类 7 维
 *   单类布局：[剩余题数_sc, power_need, wis_need, cha_need, luck_need, agi_need, hp_need]
 *   - type1：132 ~ 138
 *   - type2：139 ~ 145
 *   - type3：146 ~ 152
 *   - type4：153 ~ 159
 *   - type5：160 ~ 166
 *
 * - [167 ~ 201] (35)：arch_gain，剩余题库收益画像，共 5 类，每类 7 维
 *   单类布局：[剩余题数_sc, power_gain, wis_gain, cha_gain, luck_gain, agi_gain, hp_gain]
 *   - type1：167 ~ 173
 *   - type2：174 ~ 180
 *   - type3：181 ~ 187
 *   - type4：188 ~ 194
 *   - type5：195 ~ 201
 *
 * @note
 *  1. 该布局对应 USE_HISTORY = false，USE_PRESSURE = false；开关改动会破坏特征对齐。
 *  2. arch 空分类：后 6 维必须填充 -1.0，剩余比例通道正常保留 0.0。
 *  3. 所有缩放、归一规则必须和 Python `to_vec()` 严格一致，否则推理结果错位。
 */
int flash_predict(const float *flat_input, float *p_out, float *v_out);

#endif // net.h
