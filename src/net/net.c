/* 拼装小型Transformer神经网络 */

#include <stdio.h>
#include "ops.h"
#include "./weight/weights.h"
#include "net.h"

/*
 * State → 网络输入。唯一入口，训练采样与在线推理共用。
 *
 * 存储层（buffer / 存档）使用扁平向量 to_vec；网络层使用 token 化的 split_tokens。
 * 二者是同一批数据的两种切法，不改变任何量纲。
 *
 * token 规格按名字生成（见 GROUPS / TOKEN_NAMES），所有下标使用 IDX[...] 查询，
 * net.forward 内部不写死位置。新增 / 删除 token 不会悄无声息地造成错位。
 *
 * 分组清单（GROUPS）：
 *
 *   q          41    当前题目 one-hot                     → 1 个 token
 *   need       4×6   每个选项的 6 维门槛                  → 4 个 token
 *   succ       4×7   [成功得分] + success 属性变化         → 4 个 token
 *   fail       4×7   [失败得分] + fail 属性变化           → 4 个 token
 *   health     1     生命值                               → 1 个 token
 *   other      5     power / wisdom / charm / luck / agility
 *                                                        → 1 个 token
 *   round      4     round_sc / score_sc / target_sc / remain_round_sc
 *                                                        → 1 个 token
 *   step       1     已答题数 / 10
 *                                                        → 1 个 token
 *   arch_need  5×7   [剩余题数_sc, power 类 need 画像(6)]，一共 5 个 token
 *   arch_gain  5×7   同上，替换为成功增益画像              → 5 个 token
 *
 * arch_*：针对还没有出现的题目，按照主属性（门槛 6 维中最大那一维）聚合得到画像；
 * 在 State 上增量维护。把“第 10 题可能需要智慧 7”这类信息，从只能靠历史硬记，
 * 改为显式输入网络。
 *
 * 重要：原始 20 维选项特征布局：
 *   [need6][succ_score][fail_score][succ_chg6][fail_chg6]
 *
 * 因此 succ / fail 并不是连续切片：
 *   succ：取 [6] + [8:14]
 *   fail：取 [7] + [14:20]
 *
 * 总 token 数量：27
 * 扁平输入向量总长：INPUT_DIM = 202（USE_PRESSURE = false）
 */

#define NET_D_MODEL 128
#define NET_N_HEAD 4
#define NET_HEAD_DIM 32
#define NET_NUM_TOKENS 27

// 真实的全局与选项索引
static const int IDX_GLOBAL[15] = { 0, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26 };
static const int IDX_OPT[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };

// 硬编码 TOKEN_TYPES，用于查 embedding
static const int TOKEN_TYPES[27] = { 0, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 6, 7, 8, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10 };

// 27 个分组的输入维度 
static const int GROUP_DIMS[27] = {
    41,                 // 0: q
    6, 6, 6, 6,         // 1-4: need
    7, 7, 7, 7,         // 5-8: succ
    7, 7, 7, 7,         // 9-12: fail
    1,                  // 13: health
    5,                  // 14: other
    4,                  // 15: round
    1,                  // 16: step
    7, 7, 7, 7, 7,      // 17-21: arch_need
    7, 7, 7, 7, 7       // 22-26: arch_gain
};

// ======== 全局 Buffer ========
static float buf_x[NET_NUM_TOKENS][NET_D_MODEL];
static float buf_norm[NET_NUM_TOKENS][NET_D_MODEL];
static float buf_qkv[NET_NUM_TOKENS][NET_D_MODEL * 3];
static float buf_attn_out[NET_NUM_TOKENS][NET_D_MODEL];
static float buf_ff_hidden[NET_NUM_TOKENS][NET_D_MODEL * 2];

static void transformer_layer_forward(float input_x[NET_NUM_TOKENS][NET_D_MODEL],
    float output_x[NET_NUM_TOKENS][NET_D_MODEL])
{
    // 1. LayerNorm(norm_first = True)
    for (int i = 0; i < NET_NUM_TOKENS; ++i)
    {
        layernorm(input_x[i],
            encoder_layers_0_norm1_weight,
            encoder_layers_0_norm1_bias,
            buf_norm[i],
            NET_D_MODEL);
    }

    // 2. Multi-Head Attention
    // 2.1 Q,K,V(in_proj:[384,128])
    for (int i = 0; i < NET_NUM_TOKENS; ++i)
    {
        // Pytorch 把 Q / K / V 三个投影合并成一个大 Linear
        linear(buf_norm[i],
            encoder_layers_0_self_attn_in_proj_weight,
            encoder_layers_0_self_attn_in_proj_bias,
            buf_qkv[i],
            NET_D_MODEL,
            NET_D_MODEL * 3);
    }

    // 2.2 Calculate Attention
    for (int i = 0; i < NET_NUM_TOKENS; ++i) // 遍历Query（当前token）
    {
        for (int h = 0; h < NET_N_HEAD; ++h) // 遍历4个Head
        {
            int offset = h * NET_HEAD_DIM;
            float scores[NET_NUM_TOKENS];
            float max_score = -1e9;

            // Q * K^T
            for (int j = 0; j < NET_NUM_TOKENS; ++j) // 遍历Key (所有token）
            {
                float dot = 0.0f;
                for (int k = 0; k < NET_HEAD_DIM; ++k)
                {
                    // Q 在前 128，K 在中间 128，V 在最后 128
                    float q_val = buf_qkv[i][offset + k];
                    float k_val = buf_qkv[j][NET_D_MODEL + offset + k];
                    dot += q_val * k_val;
                }
                dot /= sqrtf(NET_HEAD_DIM); // 缩放
                scores[j] = dot;
                if (dot > max_score)
                    max_score = dot;
            }

            // Softmax
            float sum = 0.0f;
            for (int j = 0; j < NET_NUM_TOKENS; ++j)
            {
                scores[j] = expf(scores[j] - max_score);
                sum += scores[j];
            }
            for (int j = 0; j < NET_NUM_TOKENS; ++j)
                scores[j] /= sum;

            // Multiply V
            for (int j = 0; j < NET_HEAD_DIM; ++j)
            {
                float sum_v = 0.0f;
                for (int k = 0; k < NET_NUM_TOKENS; ++k)
                {
                    float v_val = buf_qkv[k][2 * NET_D_MODEL + offset + j];
                    sum_v += scores[k] * v_val;
                }
                buf_attn_out[i][offset + j] = sum_v;
            }
        }
    }

    // 2.3 Out Projection
    for (int i = 0; i < NET_NUM_TOKENS; ++i)
    {
        linear(buf_attn_out[i],
            encoder_layers_0_self_attn_out_proj_weight,
            encoder_layers_0_self_attn_out_proj_bias,
            buf_norm[i],
            NET_D_MODEL,
            NET_D_MODEL);
    }

    // 2.4 第一层残差链接
    for (int i = 0; i < NET_NUM_TOKENS; ++i)
        add_vec(input_x[i], buf_norm[i], input_x[i], NET_D_MODEL);

    // 3. 第二层LayerNorm + FFN
    for (int i = 0; i < NET_NUM_TOKENS; ++i)
    {
        // 3.1 LayerNorm 2
        layernorm(input_x[i],
            encoder_layers_0_norm2_weight,
            encoder_layers_0_norm2_bias,
            buf_norm[i],
            NET_D_MODEL);

        // 3.2 FFN 第一层 (Linear -> ReLU)
        // 维度扩展: 128 -> 256
        linear(buf_norm[i],
            encoder_layers_0_linear1_weight,
            encoder_layers_0_linear1_bias,
            buf_ff_hidden[i],
            NET_D_MODEL,
            NET_D_MODEL * 2);

        relu_inplace(buf_ff_hidden[i], NET_D_MODEL * 2);

        // 3.3 FFN 第二层 (Linear)
        // 维度压缩: 256 -> 128
        linear(buf_ff_hidden[i],
            encoder_layers_0_linear2_weight,
            encoder_layers_0_linear2_bias,
            buf_norm[i],
            NET_D_MODEL * 2,
            NET_D_MODEL);

        // 3.4 第二层残差
        add_vec(input_x[i], buf_norm[i], output_x[i], NET_D_MODEL);
    }
}

/*
 * projs.0: (128,41) q(当前题)              1 token
 * projs.1: (128,6) need(门槛)              4 tokens 权重共享
 * projs.2: (128,7) succ(成功收益)          4 tokens 权重共享
 * projs.3: (128,7) fail(失败惩罚)          4 tokens 权重共享
 * projs.4: (128,1) health(生命值)          1 token
 * projs.5: (128,5) other(五维属性)         1 token
 * projs.6: (128,4) round(回合特征)         1 token
 * projs.7: (128,1) step(步数)              1 token
 * projs.8: (128,7) arch_need(未来需求画像) 5 tokens 权重共享
 * projs.9: (128,7) arch_gain(未来收益画像) 5 tokens 权重共享
 */

 // 10 个投影层的指针
static const float *projs_weights[10] = {
    projs_0_weight, projs_1_weight, projs_2_weight, projs_3_weight, projs_4_weight,
    projs_5_weight, projs_6_weight, projs_7_weight, projs_8_weight, projs_9_weight
};
static const float *projs_biases[10] = {
    projs_0_bias, projs_1_bias, projs_2_bias, projs_3_bias, projs_4_bias,
    projs_5_bias, projs_6_bias, projs_7_bias, projs_8_bias, projs_9_bias
};

// 映射表：[proj索引, 该组产生几个token, 该组的输入维度]
static const int GROUP_MAP[10][3] = {
    {0, 1, 41}, // q
    {1, 4, 6},  // need (4个选项)
    {2, 4, 7},  // succ (4个选项)
    {3, 4, 7},  // fail (4个选项)
    {4, 1, 1},  // health
    {5, 1, 5},  // other
    {6, 1, 4},  // round
    {7, 1, 1},  // step
    {8, 5, 7},  // arch_need (5个 token)
    {9, 5, 7}   // arch_gain (5个 token)
};

// 核心前向传播
// input: 长度为 INPUT_DIM (202)
// output: logits [4], v [1]
static void net_forward(const float *flat_input, float *logits, float *v_out) {

    // ---------- 1. 输入投影 (Projs) ----------
    int input_offset = 0;
    int token_idx = 0;

    for (int g = 0; g < 10; g++) {
        int p_idx = GROUP_MAP[g][0];
        int n_tokens = GROUP_MAP[g][1];
        int in_dim = GROUP_MAP[g][2];

        for (int t = 0; t < n_tokens; t++) {
            // 注意：这里是对每一组进行循环，用同样的权重处理不同的 token
            linear(flat_input + input_offset,
                projs_weights[p_idx], projs_biases[p_idx],
                buf_x[token_idx], in_dim, NET_D_MODEL);

            input_offset += in_dim;
            token_idx++;
        }
    }

    // ---------- 2. 加上 type_emb ----------
    for (int i = 0; i < NET_NUM_TOKENS; i++) {
        int type_id = TOKEN_TYPES[i];
        for (int j = 0; j < NET_D_MODEL; j++) {
            buf_x[i][j] += type_emb_weight[type_id * NET_D_MODEL + j];
        }
    }

    // ---------- 3. 过 Transformer ----------
    static float x_enc[NET_NUM_TOKENS][NET_D_MODEL];
    transformer_layer_forward(buf_x, x_enc);

    // ---------- 4. 提取状态并过 State MLP ----------
    // 拼接 15 个全局 token + mean + max = 17 * 128
    static float state_in[17 * NET_D_MODEL];
    int state_ptr = 0;

    // 15 个全局 token 展平
    for (int i = 0; i < 15; i++) {
        memcpy(state_in + state_ptr, x_enc[IDX_GLOBAL[i]], NET_D_MODEL * sizeof(float));
        state_ptr += NET_D_MODEL;
    }

    // 12 个选项 token 的 mean 和 max
    static float opt_mean[NET_D_MODEL] = { 0 };
    memset(opt_mean, 0, sizeof(opt_mean));

    static float opt_max[NET_D_MODEL];
    for (int j = 0; j < NET_D_MODEL; j++) opt_max[j] = -1e9f;

    for (int i = 0; i < 12; i++) {
        int idx = IDX_OPT[i];
        for (int j = 0; j < NET_D_MODEL; j++) {
            float val = x_enc[idx][j];
            opt_mean[j] += val;
            if (val > opt_max[j]) opt_max[j] = val;
        }
    }
    for (int j = 0; j < NET_D_MODEL; j++) opt_mean[j] /= 12.0f;

    memcpy(state_in + state_ptr, opt_mean, NET_D_MODEL * sizeof(float)); state_ptr += NET_D_MODEL;
    memcpy(state_in + state_ptr, opt_max, NET_D_MODEL * sizeof(float)); state_ptr += NET_D_MODEL;

    // 过 state_mlp (2176 -> 128)
    static float h_state[NET_D_MODEL];
    linear(state_in, state_mlp_0_weight, state_mlp_0_bias, h_state, 2176, NET_D_MODEL);
    relu_inplace(h_state, NET_D_MODEL);

    // ---------- 5. 计算 V (废弃的 s_head 直接删掉) ----------
    static float v_hidden[64];
    linear(h_state, v_head_0_weight, v_head_0_bias, v_hidden, NET_D_MODEL, 64);
    relu_inplace(v_hidden, 64);
    float v_raw;
    linear(v_hidden, v_head_2_weight, v_head_2_bias, &v_raw, 64, 1);
    *v_out = 1.0f / (1.0f + expf(-v_raw)); // Sigmoid

    // ---------- 6. 计算 4 个选项的 Logits ----------
    for (int i = 0; i < 4; i++) {
        // 索引映射：need(1~4), succ(5~8), fail(9~12)
        int idx_need = 1 + i;
        int idx_succ = 5 + i;
        int idx_fail = 9 + i;

        static float opt_cat[4 * NET_D_MODEL];
        memcpy(opt_cat, x_enc[idx_need], NET_D_MODEL * sizeof(float));
        memcpy(opt_cat + NET_D_MODEL, x_enc[idx_succ], NET_D_MODEL * sizeof(float));
        memcpy(opt_cat + 2 * NET_D_MODEL, x_enc[idx_fail], NET_D_MODEL * sizeof(float));
        memcpy(opt_cat + 3 * NET_D_MODEL, h_state, NET_D_MODEL * sizeof(float)); // 广播 h_state

        static float p_hidden[64];
        // p_head 第一层: (4*128) -> 64
        linear(opt_cat, p_head_0_weight, p_head_0_bias, p_hidden, 4 * NET_D_MODEL, 64);
        relu_inplace(p_hidden, 64);

        // p_head 第二层: 64 -> 1
        linear(p_hidden, p_head_2_weight, p_head_2_bias, &logits[i], 64, 1);
    }
}

// 接口，对外输出最佳选项，闪电模式
int flash_predict(const float *flat_input, float *p_out, float *v_out)
{
    float logits[4];
    float tmp_v_out;

    if (!v_out) v_out = &tmp_v_out;

    net_forward(flat_input, logits, v_out);

    int best_action = 0;
    float max_logit = logits[0];
    for (int i = 1; i < 4; i++) {
        if (logits[i] > max_logit) {
            max_logit = logits[i];
            best_action = i;
        }
    }

    if (p_out != NULL) {
        float sum = 0.0f;
        for (int i = 0; i < 4; i++) {
            p_out[i] = expf(logits[i] - max_logit); // 减去 max 防溢出
            sum += p_out[i];
        }
        for (int i = 0; i < 4; i++) {
            p_out[i] /= sum;
        }
    }

    return best_action;
}
