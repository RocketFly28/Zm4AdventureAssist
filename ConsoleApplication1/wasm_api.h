#ifndef WASM_API_H
#define WASM_API_H
#include "common.h"

#ifdef WASM
#include "Adventure.h"

#ifdef __cplusplus
extern "C" {
#endif

	GameContext *wasm_game_create(int role_id);
	/* 从局面快照续档创建一局。s 必须是 JS 用 _malloc 分配的
	 * sizeof(GameSnapshot) 字节并按 Adventure.h 中的布局填好的指针。 */
	GameContext *wasm_game_create_form(GameSnapshot *s);
	/* 返回 sizeof(GameSnapshot)，JS 侧用它决定 _malloc 多少字节 */
	int wasm_game_snapshot_size(void);
	/* 在已有 GameContext 上原地改写局面；成功返回 0，失败返回 -1。
	 * s->ctx 必须指向 create / create_form 返回的那个上下文。 */
	int wasm_game_ctx_modify(GameSnapshot *s);
	/* 由 JS 侧把 buf 填成 GameSnapshot（避免手写偏移量出错）；
	 * qid_selected 传 15 字节的 0/1 数组指针，可传 NULL 表示全部未选；
	 * ctx 填目标 GameContext 指针，modify 时必填，create_form 时会被忽略。
	 * 返回 0 成功，-1 参数非法 */
	int wasm_game_snapshot_fill(GameSnapshot *buf, int health, int power,
	                            int wisdom, int charm, int luck, int agility,
	                            int done_cnt, int score,
	                            const uint8_t *qid_selected,
	                            GameContext *ctx);
	void wasm_game_destroy(GameContext *ctx);
	const Attrs *wasm_game_get_attrs(GameContext *ctx);
	int wasm_game_get_done_cnt(GameContext *ctx);
	int wasm_game_get_total_score(GameContext *ctx);
	int wasm_game_ctx_find_qidx_by_id(const GameContext *ctx, int question_id);
	int wasm_game_evaluate(GameContext *ctx, int q_idx, int target_T, OptionEval *out4);
	int wasm_game_do_step(GameContext *ctx, int q_idx, int opt, StepResult *out_res);

	/* =======================================================================
	 *                      冲刺奖励模式（7 局冲刺）
	 *
	 * 玩法：连续打 7 局，每局 10 题；每局开始属性重置为该角色初始值。
	 *       7 局满分 700，主目标 620（允许丢 80），保底目标 480（允许丢 220）。
	 * 前端只负责"记录已打完几局、累计多少分"，所有概率/策略都在内核算。
	 * ======================================================================= */
#define SPRINT_ROUNDS       7     /* 总局数 */
#define SPRINT_TARGET_MAIN  620   /* 主目标分 */
#define SPRINT_TARGET_SAFE  480   /* 保底目标分 */

	GameContext *wasm_game_create_sprint(int role_id);

	GameContext *wasm_game_create_sprint_form(GameSnapshot *s);

	/* ABI 版本号。前端启动时校验，用于发现 wasm 与小程序包版本不同步。
	 * 每次修改下列任一接口的签名/语义后 +1。 */
	int wasm_abi_version(void);

	/* 【纯函数·无 ctx·无副作用】冲刺胜率查表。
	 *
	 * 参数（JS 期望接受的取值）：
	 *   completed_rounds : 已完成局数，取值范围 0..6。
	 *                      用户输入"现在是第几局"(1..7)时传"局数-1"；全新整局传 0。
	 *   total_score      : 已完成局数的累计总得分。
	 *                      宽校验区间 [0, 100 * completed_rounds]；
	 *                      未打完任何一局传 0。
	 *                      （区间刻意放宽到 0：允许用户中途弃题、弃局，
	 *                        因此不做"单局 48~100"的下限校验。）
	 *   out              : 由 JS 用 _malloc(16) 分配、长度 >= 2 的 double 数组。
	 *                      out[0] = 冲刺 620 的最大达标概率，取值 [0,1]
	 *                      out[1] = 冲刺 480 的最大达标概率，取值 [0,1]
	 *                      概率为 0 表示该目标已无解（前端据此切保底策略）。
	 * 返回：0 成功；-1 参数非法（out == NULL 或 completed_rounds 不在 0..6）。
	 */
	int wasm_sprint_winrate(int completed_rounds, int total_score, double *out);

	/* 【有 ctx】冲刺模式下对"当前局某题 4 个选项"的评估。
	 * 输出布局与 wasm_game_evaluate 完全一致：4 个 OptionEval，共 64 字节。
	 *
	 * 参数（JS 期望接受的取值）：
	 *   ctx              : 当前局的 GameContext（局内 done_cnt/score/attrs 由它维护）
	 *   q_idx            : 题目下标（语义同 wasm_game_evaluate）
	 *   completed_rounds : 已完成局数 0..6（语义同 wasm_sprint_winrate）
	 *   total_score      : 已完成局数的累计总分，不含当前局；
	 *                      宽校验区间 [0, 100 * completed_rounds]
	 *   target           : 优化目标，只接受以下三个值
	 *                        620 -> 用 win_rates80  表，最大化"最终 >= 620"的概率
	 *                        480 -> 用 win_rates220 表，最大化"最终 >= 480"的概率
	 *                        0   -> 不做达标优化，返回最大剩余期望得分；
	 *                               此时 out[i].prob_goal 返回 -1（与 evaluate 一致）
	 *   out4             : 4 个 OptionEval（JS 用 _malloc(64) 分配）
	 * 返回：0 成功；-1 参数非法（ctx/out4 为 NULL，或 completed_rounds 越界）。
	 *
	 * 内核需自行把"局内进度"折算成当前剩余的"可丢分预算"，再查对应表；
	 * 折算口径请与 dpGlobalProMax 的 able_lost_s 保持一致。
	 * 前端不关心行/列的换算，只要 target 与 out 的语义对齐即可。
	 */
	int wasm_sprint_evaluate(GameContext *ctx, int q_idx,
	                         int completed_rounds, int total_score,
	                         int target, OptionEval *out4);

#ifdef __cplusplus
}
#endif

#endif // WASM
#endif
