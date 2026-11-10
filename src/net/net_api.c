#include "net_api.h"
#include "net.h"
#include "../dpsolver/hash.h"
#include "../dpsolver/Adventure.h"

#include <string.h>

/* ===========================================================================
 * flash 模型的输入特征构造。
 *
 * 【唯一权威】aizero/features.py + aizero/game.py + aizero/norm.py。
 * 这里逐条复刻，任何一处口径变化都必须同步本文件，否则推理结果会静默跑偏。
 *
 * 202 维的布局（与 net.c 的 GROUP_MAP / net_forward 一致）：
 *
 *   [  0 ~  40]  q          当前题 one-hot（全局题号 0~40，41 维）
 *   [ 41 ~  64]  need       4 选项 × 6 维门槛
 *   [ 65 ~  92]  succ       4 选项 × [成功得分, 成功增益 6 维]
 *   [ 93 ~ 120]  fail       4 选项 × [失败得分, 失败惩罚 6 维]
 *   [121]        health     当前生命
 *   [122 ~ 126]  other      power / wisdom / charm / luck / agility
 *   [127 ~ 130]  round      round_sc / score_sc / target_sc / remain_round_sc
 *   [131]        step       已答题数 / 10
 *   [132 ~ 166]  arch_need  5 主属性 × [剩余占比_sc, need 均值 6 维]
 *   [167 ~ 201]  arch_gain  5 主属性 × [剩余占比_sc, 增益均值 6 维]
 *
 * 与 Python 的量纲口径（norm.py / config.py / meta.json）：
 *   - 属性一律 clip 到 [0, 12] 再 / 12；
 *   - 门槛 127（“必定失败”哨兵）压到 13，并 clip 到 [0, 13]；
 *   - 分数、属性增益不归一（原始值）；
 *   - score_sc = 累计分 / (100 × totalround)，训练时 totalround = 1；
 *   - target_sc = target_round / 100，训练时 target_round = 70 → 0.7；
 *   - round_sc / remain_round_sc 在 totalround = 1 时恒为 0。
 * ========================================================================= */

#define NN_ATTR_MAX        12.0f   /* config.ATTR_MAX            */
#define NN_THR_IMPOSSIBLE  13.0f   /* config.THRESHOLD_IMPOSSIBLE */
#define NN_SCORE_MAX      100.0f   /* config.SCORE_MAX            */
#define NN_TARGET_ROUND    70.0f   /* meta.json: target_round     */
#define NN_STEP_MAX         10.0f  /* config.QUESTIONS_PER_ROUND  */
#define NN_NUM_ARCH            5   /* config.NUM_ARCH             */
#define NN_NUM_ATTR            6   /* config.NUM_ATTR             */

/* 全局题号偏移：type(1~5) → 该类题在 41 维 one-hot 里的起始下标 */
static const int k_question_off[5] = { 0, 4, 14, 29, 36 };

/* 各主属性成员总数（Python 的 game.ARCH_TOTAL，由题库静态算得）。
 * 用 qid 无关的常数缓存，题库变动时必须同步 —— 与 Python 侧的 assert 等价。 */
static const float k_arch_scale[NN_NUM_ARCH] = { 29.0f, 24.0f, 32.0f, 25.0f, 27.0f };

/* ---------------------------------------------------------------------------
 * 得分口径：与内核一致，按阶段取常数（successScore / failScore[type]）。
 * data.json 已按同一口径补齐了每个选项的 score 字段，两边逐项一致。
 * ------------------------------------------------------------------------- */

static float nn_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* 取 6 维属性为 float */
static void nn_attrs6(const Attrs *a, float out[6])
{
    out[0] = (float)a->health;
    out[1] = (float)a->power;
    out[2] = (float)a->wisdom;
    out[3] = (float)a->charm;
    out[4] = (float)a->luck;
    out[5] = (float)a->agility;
}

/* ---------------------------------------------------------------------------
 * 主属性（archetype）判定，与 game.dominant_arch 一致：
 *   - health 不参评（它在题库里只有「哨兵 127 = 必失败」一种语义）；
 *   - 其余 5 维门槛全为 0 → -1（没有门槛的选项不携带信息）；
 *   - 并列最大取靠前的一个（即 np.argmax 的默认行为）。
 * ------------------------------------------------------------------------- */
static int nn_dominant_arch(const Option *opt)
{
    const float body[NN_NUM_ARCH] = {
        (float)opt->successAttrs.power,
        (float)opt->successAttrs.wisdom,
        (float)opt->successAttrs.charm,
        (float)opt->successAttrs.luck,
        (float)opt->successAttrs.agility,
    };
    float best = 0.0f;
    int   idx  = -1;
    int   i;

    for (i = 0; i < NN_NUM_ARCH; ++i)
    {
        if (body[i] > best) { best = body[i]; idx = i; }
    }
    return idx;
}

/* ---------------------------------------------------------------------------
 * arch 累加器：把「一道题」并入画像。
 *
 * 与 Python game._build_arch_tables / arch_initial 的口径一致 —— 关键是
 * **先按题内成员求均值，再按题累加**（每道题对某个主属性只贡献 0/1 的计数和
 * 一个均值），而不是把所有选项的原始值混在一起平均。一道题若有两个选项同属
 * 一个主属性，两种算法结果不同；题库里有 6/41 道题是这种情况。
 * ------------------------------------------------------------------------- */
static void nn_arch_add_question(const Question *q,
                                 int cnt[NN_NUM_ARCH],
                                 float sum_need[NN_NUM_ARCH][NN_NUM_ATTR],
                                 float sum_gain[NN_NUM_ARCH][NN_NUM_ATTR])
{
    int   members[NN_NUM_ARCH] = { 0 };
    float need[NN_NUM_ARCH][NN_NUM_ATTR] = { { 0 } };
    float gain[NN_NUM_ARCH][NN_NUM_ATTR] = { { 0 } };
    int   o, a, j;

    for (o = 0; o < (int)OPT_PER_QUESTION; ++o)
    {
        const Option *opt = &q->options[o];
        float thr[6], awd[6];

        a = nn_dominant_arch(opt);
        if (a < 0) continue;

        nn_attrs6(&opt->successAttrs, thr);
        nn_attrs6(&opt->successAward, awd);

        members[a] += 1;
        for (j = 0; j < NN_NUM_ATTR; ++j)
        {
            need[a][j] += nn_clampf(thr[j], 0.0f, NN_THR_IMPOSSIBLE);
            gain[a][j] += awd[j];
        }
    }

    for (a = 0; a < NN_NUM_ARCH; ++a)
    {
        float m;
        if (members[a] == 0) continue;      /* 该题不属于这类，跳过 */
        m = (float)members[a];
        cnt[a] += 1;                        /* 按「题」计数，不是按选项 */
        for (j = 0; j < NN_NUM_ATTR; ++j)
        {
            sum_need[a][j] += need[a][j] / m;
            sum_gain[a][j] += gain[a][j] / m;
        }
    }
}

/* ---------------------------------------------------------------------------
 * 输出一块 arch（35 维）：[cnt/scale, 6 维均值] × 5
 *
 * 空类用 -1 哨兵（与 features._arch_block 一致）：门槛全 0 在题库里不存在，
 * 所以均值全 0 只可能来自「该类题已抽空」。填 0 会让两种含义无法区分。
 * ------------------------------------------------------------------------- */
static void nn_arch_emit(const int cnt[NN_NUM_ARCH],
                         const float sum_need[NN_NUM_ARCH][NN_NUM_ATTR],
                         const float sum_gain[NN_NUM_ARCH][NN_NUM_ATTR],
                         float *out_need, float *out_gain)
{
    int a, j;

    for (a = 0; a < NN_NUM_ARCH; ++a)
    {
        float c     = (float)cnt[a];
        float denom = c > 0.0f ? c : 1.0f;
        float alive = c > 0.0f ? 1.0f : 0.0f;
        int   base  = a * (NN_NUM_ATTR + 1);

        out_need[base] = c / k_arch_scale[a];   /* 还剩几成（∈[0,1]） */
        out_gain[base] = c / k_arch_scale[a];

        for (j = 0; j < NN_NUM_ATTR; ++j)
        {
            out_need[base + 1 + j] = sum_need[a][j] / denom * alive + (alive - 1.0f);
            out_gain[base + 1 + j] = sum_gain[a][j] / denom * alive + (alive - 1.0f);
        }
    }
}

/* ---------------------------------------------------------------------------
 * 拼装「还没出现的题」的画像。
 *
 * 剩余题集 = 当前阶段还没答的题 + 之后所有阶段的全部题目。
 * （题目顺序由 SCHEDULE 固定：每阶段只有在本阶段槽位没用完时才可能被抽到，
 *   已结束的阶段整类作废，所以这个选取方式与 Python 的 remaining 判定等价。）
 *
 * 特例：最后一题（done_cnt == 9）Python 在 game.step 里把整块清零
 * （arch_cnt=0，均值随之变成 -1 哨兵），这里必须照做，否则最后一步的输入
 * 会多出一批「幻影题」。
 * ------------------------------------------------------------------------- */
static void nn_build_arch(GameContext *ctx, int done_cnt, int type,
                          float *out_need, float *out_gain)
{
    int   cnt[NN_NUM_ARCH] = { 0 };
    float sum_need[NN_NUM_ARCH][NN_NUM_ATTR];
    float sum_gain[NN_NUM_ARCH][NN_NUM_ATTR];
    int   i, g;

    memset(sum_need, 0, sizeof(sum_need));
    memset(sum_gain, 0, sizeof(sum_gain));

    if (done_cnt < 9)
    {
        uint16_t mask = game_ctx_get_mask(ctx);
        const Question *cur = question_db_for_type(type);

        for (i = 0; i < totalQs[type]; ++i)
        {
            if (mask & convert_group_mask(i)) continue;    /* 本阶段已答 */
            nn_arch_add_question(&cur[i], cnt, sum_need, sum_gain);
        }
        for (g = type + 1; g < 5; ++g)
        {
            const Question *later = question_db_for_type(g);
            for (i = 0; i < totalQs[g]; ++i)
                nn_arch_add_question(&later[i], cnt, sum_need, sum_gain);
        }
    }

    nn_arch_emit(cnt, sum_need, sum_gain, out_need, out_gain);
}

/* ---------------------------------------------------------------------------
 * 对外接口：填 202 维特征 → 前向 → 4 个选项的 P 与当前节点期望得分
 *
 * 返回模型选出的最优选项下标 0~3（<0 表示推理失败）。
 * out[i].prob_goal     = 四个选项各自命中目标的概率（softmax 后的值）
 * out[i].expect_remain = 当前节点的预期得分（四个选项相同，×100 折算成分数）
 *
 * target_T 不参与计算：flash 模型训练时 target_sc 恒为 0.7（target_round=70），
 * 是一个与目标分无关的常数输入。
 * ------------------------------------------------------------------------- */
int flash_evaluate_options(GameContext *ctx, int q_idx, int target_T, OptionEval out[4])
{
    float flat_input[202] = { 0 };
    float p_out[4];
    float v_out = 0.0f;
    const Question *q;
    const Attrs *cur;
    int done_cnt, type, i, j;
    int best;

    (void)target_T;

    done_cnt = game_ctx_get_done_cnt(ctx);
    type     = judgeGroup(done_cnt);
    if (type < 0) return -1;                                  /* 本局已结束 */
    if (q_idx < 0 || q_idx >= totalQs[type]) return -1;       /* 下标越界 */

    q   = question_db_for_type(type);
    q   = &q[q_idx];

    /* ---- [0 ~ 40] 当前题 one-hot（全局题号） ---- */
    flat_input[q_idx + k_question_off[type]] = 1.0f;

    /* ---- [41 ~ 120] 四个选项的门槛 / 奖励 / 惩罚 ---- */
    for (i = 0; i < (int)OPT_PER_QUESTION; ++i)
    {
        const Option *opt = &q->options[i];
        float need6[6], succ6[6], fail6[6];

        nn_attrs6(&opt->successAttrs, need6);
        nn_attrs6(&opt->successAward, succ6);
        nn_attrs6(&opt->failPunish,   fail6);

        for (j = 0; j < NN_NUM_ATTR; ++j)
        {
            /* need：哨兵 127 压到 13，并 clip 到 [0, 13] */
            flat_input[41 + 6 * i + j] =
                nn_clampf(need6[j], 0.0f, NN_THR_IMPOSSIBLE);
            /* succ：增益原始值（可正可负，不归一） */
            flat_input[66 + 7 * i + j] = succ6[j];
            /* fail：惩罚原始值（题库里是负数） */
            flat_input[94 + 7 * i + j] = fail6[j];
        }

        /* 得分：与内核一致，按阶段取常数 */
        flat_input[65 + 7 * i] = (float)successScore[type];
        flat_input[93 + 7 * i] = (float)failScore[type];
    }

    /* ---- [121 ~ 126] 当前属性：clip 到 [0,12] 再 /12 ---- */
    cur = game_ctx_get_attrs(ctx);
    flat_input[121] =
        nn_clampf((float)cur->health, 0.0f, NN_ATTR_MAX) / NN_ATTR_MAX;
    for (j = 0; j < NN_NUM_ARCH; ++j)
    {
        float v;
        switch (j)
        {
        case 0:  v = (float)cur->power;   break;
        case 1:  v = (float)cur->wisdom;  break;
        case 2:  v = (float)cur->charm;   break;
        case 3:  v = (float)cur->luck;    break;
        default: v = (float)cur->agility; break;
        }
        flat_input[122 + j] = nn_clampf(v, 0.0f, NN_ATTR_MAX) / NN_ATTR_MAX;
    }

    /* ---- [127 ~ 131] round / step ---- */
    flat_input[127] = 0.0f;                    /* round_sc：totalround=1 → 0 */
    flat_input[128] = (float)game_ctx_get_total_score(ctx) / NN_SCORE_MAX;
    flat_input[129] = NN_TARGET_ROUND / NN_SCORE_MAX;   /* 0.7 */
    flat_input[130] = 0.0f;                    /* remain_round_sc → 0 */
    flat_input[131] = (float)done_cnt / NN_STEP_MAX;

    /* ---- [132 ~ 201] arch_need / arch_gain ---- */
    nn_build_arch(ctx, done_cnt, type, &flat_input[132], &flat_input[167]);

    /* ---- 前向 ---- */
    best = flash_predict(flat_input, p_out, &v_out);

    for (i = 0; i < 4; ++i)
    {
        out[i].prob_goal     = (double)p_out[i];
        out[i].expect_remain = (double)(v_out * 100.0f);  /* v∈[0,1] → 分数 */
    }
    return best;
}
