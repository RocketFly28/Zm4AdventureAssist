#include "common.h"

#ifdef WASM
#include <emscripten.h>
#include "Adventure.h"
#include "wasm_api.h"

// 包装创建一局游戏
EMSCRIPTEN_KEEPALIVE
GameContext *wasm_game_create(int role_id)
{
    // JS传数字，转成枚举RoleId
    return game_ctx_create((RoleId)role_id);
}

// 从局面快照续档创建一局（属性会被裁剪到合法区间）
EMSCRIPTEN_KEEPALIVE
GameContext *wasm_game_create_form(GameSnapshot *s)
{
    if (!s) return NULL;
    return game_ctx_create_form(s);
}

// 返回 sizeof(GameSnapshot)，JS 用它决定要 _malloc 多少字节
EMSCRIPTEN_KEEPALIVE
int wasm_game_snapshot_size(void)
{
    return (int)sizeof(GameSnapshot);
}

// 在已有 GameContext 上原地改写局面（不新建对象、不重建 memo、不改 role_id）
EMSCRIPTEN_KEEPALIVE
int wasm_game_ctx_modify(GameSnapshot *s)
{
    if (!s) return -1;
    return game_ctx_modify(s);
}

// 在 wasm 侧填充快照，避免 JS 手写偏移量出错
EMSCRIPTEN_KEEPALIVE
int wasm_game_snapshot_fill(GameSnapshot *buf, int health, int power,
                            int wisdom, int charm, int luck, int agility,
                            int done_cnt, int score,
                            const uint8_t *qid_selected,
                            GameContext *ctx)
{
    if (!buf) return -1;
    if (done_cnt < 0 || done_cnt > 9) return -1;

    for (int i = 0; i < 15; ++i)
        buf->qid_selected[i] = (qid_selected && qid_selected[i]) ? 1 : 0;

    buf->attrs.health  = (int8_t)health;
    buf->attrs.power   = (int8_t)power;
    buf->attrs.wisdom  = (int8_t)wisdom;
    buf->attrs.charm   = (int8_t)charm;
    buf->attrs.luck    = (int8_t)luck;
    buf->attrs.agility = (int8_t)agility;
    buf->done_cnt      = (uint8_t)done_cnt;
    buf->score         = score;
    buf->ctx           = ctx;
    return 0;
}

// 销毁游戏
EMSCRIPTEN_KEEPALIVE
void wasm_game_destroy(GameContext *ctx)
{
    if (ctx)
    {
        game_ctx_destroy(ctx);
    }
}

// 获取当前六维属性
EMSCRIPTEN_KEEPALIVE
const Attrs *wasm_game_get_attrs(GameContext *ctx)
{
    if (!ctx) return NULL;
    return game_ctx_get_attrs(ctx);
}

// 获取已完成题数
EMSCRIPTEN_KEEPALIVE
int wasm_game_get_done_cnt(GameContext *ctx)
{
    if (!ctx) return -1;
    return game_ctx_get_done_cnt(ctx);
}

// 获取累计总分
EMSCRIPTEN_KEEPALIVE
int wasm_game_get_total_score(GameContext *ctx)
{
    if (!ctx) return -1;
    return game_ctx_get_total_score(ctx);
}

// 通过question_id查询q_idx
EMSCRIPTEN_KEEPALIVE
int wasm_game_ctx_find_qidx_by_id(const GameContext *ctx, int question_id)
{
    if (!ctx) return -1;
    return game_ctx_find_qidx_by_id(ctx, question_id);
}

// 评估4个选项期望得分（正好是你前端按钮的预计得分！）
// 注意：JS分配数组传给C，或者C返回指针给JS读取
EMSCRIPTEN_KEEPALIVE
int wasm_game_evaluate(GameContext *ctx, int q_idx, int target_T, OptionEval *out4)
{
    if (!ctx || !out4) return -1;
    return game_evaluate_options(ctx, q_idx, target_T, out4);
}

// 执行一步选择，推进游戏
EMSCRIPTEN_KEEPALIVE
int wasm_game_do_step(GameContext *ctx, int q_idx, int opt, StepResult *out_res)
{
    if (!ctx || !out_res) return -1;
    return game_do_step(ctx, q_idx, opt, out_res);
}

/* =======================================================================
 *                     冲刺奖励模式（待内核补全实现）
 *
 * 下面三个函数目前只是"不崩的占位实现"，接口签名与参数语义见 wasm_api.h。
 * 前端已经按这些签名调用，内核补全后无需改前端。
 * ======================================================================= */

// 新增接口函数，创建冲刺模式内核
// 注意：冲刺模式下 memo 哈希表与评估模式不通用（proMax480/proMax620 是独立表），
//       因此由内核侧的 game_ctx_create_sprint 负责建好冲刺专用的 memo。
EMSCRIPTEN_KEEPALIVE
GameContext *wasm_game_create_sprint(int role_id)
{
    // JS传数字，转成枚举RoleId
    return game_ctx_create_sprint((RoleId)role_id);
}

// 从局面快照续档创建一局冲刺模式（属性会被裁剪到合法区间）
EMSCRIPTEN_KEEPALIVE
GameContext *wasm_game_create_sprint_form(GameSnapshot *s)
{
    if (!s) return NULL;
    return game_ctx_create_sprint_form(s);
}

#define SPRINT_MAX_LOSE_MAIN  80    /* 700 - 620 */
#define SPRINT_MAX_LOSE_SAFE  220   /* 700 - 480 */

// ABI 版本号：改接口后 +1
EMSCRIPTEN_KEEPALIVE
int wasm_abi_version(void)
{
    return GAME_VERSION;
}

// 冲刺胜率查表：占位实现返回 0 概率
EMSCRIPTEN_KEEPALIVE
int wasm_sprint_winrate(int completed_rounds, int total_score, double *out)
{
    if (!out) return -1;
    if (completed_rounds < 0 || completed_rounds > 6) return -1;

    /* -----------------------------------------------------------------
     * TODO(内核)：请补全为真实查表。参考换算（口径以 dpGlobalProMax 为准）：
     *
     *   int k = completed_rounds;                       // 已完成局数
     *   int S = total_score;                            // 已完成局累计分
     *   可丢分预算（列）：budget = MAX_LOSE - (100 * k - S)
     *     - 目标 620（MAX_LOSE = SPRINT_MAX_LOSE_MAIN = 80）
     *     - 目标 480（MAX_LOSE = SPRINT_MAX_LOSE_SAFE = 220）
     *     - budget < 0  -> 该目标已无解，概率取 0
     *   行（局数）索引：按 dpCal.c 既有约定自行确定（win_rates 是 static，
     *     需要在本文件或 dpCal 侧补一个可访问的查询函数）
     *
     * 注意：本文件当前看不到 win_rates80 / win_rates220（它们在 dpCal.c 内
     * 是 static const），建议在 dpCal.c 里新增一个非 static 的查询函数，
     * 由这里调用，避免把这两张表暴露到公共头文件。
     * ---------------------------------------------------------------- */
    out[0] = 0.0;
    out[1] = 0.0;
    return 0;
}

// 冲刺模式选项评估：
//   target == 620 -> 用 win_rates80  表，out[i].prob_goal = "最终 >= 620" 的全局概率
//   target == 480 -> 用 win_rates220 表，out[i].prob_goal = "最终 >= 480" 的全局概率
//   target == 0   -> 不做达标优化，prob_goal 返回 -1，只给最大剩余期望得分
// 输出布局与 wasm_game_evaluate 完全一致（4 个 OptionEval = 64 字节）。
EMSCRIPTEN_KEEPALIVE
int wasm_sprint_evaluate(GameContext *ctx, int q_idx, int completed_rounds,
                         int total_score, int target, OptionEval *out4)
{
    if (!ctx || !out4) return -1;
    return game_evaluate_options_sprint(ctx, q_idx, completed_rounds,
                                        total_score, target, out4);
}

#endif