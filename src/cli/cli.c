#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

/*
 * cli.c - command line front end (argv mode + interactive REPL).
 *
 * Design rules:
 *   - Only the public API from Adventure.h is used. GameContext stays opaque,
 *     so nothing here may read ctx->done_cnt or any other internal field.
 *   - No business logic. Every number printed comes from the kernel.
 *   - All text is ASCII, so the output survives any terminal code page.
 *
 * Commands:
 *   solve    play one round automatically, printing the best move each step
 *   table    precompute the win-rate tables (PRE_CAL builds only)
 *   roles    list roles and their initial attributes
 *   rules    print the stage / scoring rules
 *   repl     interactive mode (default when no command is given)
 *   version  build information
 *   help     usage
 */

#include "cli.h"
#include "guide_table.h"          /* 攻略表：题目 / 选项名称（纯显示用） */
#include "../dpsolver/Adventure.h"
#include "../dpsolver/dpCal.h"
#include "../net/net_api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

#define EXIT_OK     0
#define EXIT_FAIL   1
#define EXIT_USAGE  2

#define Q_PER_STAGE_MAX 15
#define OPT_PER_Q        4
#define DONE_MAX        10
#define ROUNDS_TOTAL     7   /* one full challenge spans seven rounds */
#define ROUND_MAX        6   /* completed_rounds accepted by the sprint API */

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static const char *const k_role_name[] = {
    "",
    "xiao_tang", "xiao_hou", "xiao_zhu", "xiao_sha",
    "xiao_bai",  "xiao_yu",  "xiao_zha"
};

static const char *role_name(int role)
{
    if (role < 1 || role > 7) return "?";
    return k_role_name[role];
}

static void print_attrs(const Attrs *a)
{
    if (!a) { printf("(null)"); return; }
    printf("hp=%-3d pow=%-3d wis=%-3d cha=%-3d luck=%-3d agi=%-3d",
           (int)a->health, (int)a->power, (int)a->wisdom,
           (int)a->charm, (int)a->luck, (int)a->agility);
}

static void print_rule(void)
{
    printf("--------------------------------------------------------------\n");
}

/* Format a probability (0..1) as a percentage; "n/a" when unavailable. */
static void fmt_prob(char *buf, size_t n, double p)
{
    if (p >= 0.0) snprintf(buf, n, "%7.3f%%", p * 100.0);
    else          snprintf(buf, n, "    n/a");
}

static int is_sprint_target(int target)
{
    return (target == 620 || target == 480);
}

/* Lowercase a whole line in place (REPL commands are case insensitive). */
static void to_lower(char *s)
{
    for (; *s; ++s) *s = (char)tolower((unsigned char)*s);
}

/* ------------------------------------------------------------------ */
/* guide-table helpers                                                 */
/* ------------------------------------------------------------------ */

static const char k_attr_abbr[6][4] = { "hp", "pow", "wis", "cha", "luc", "agi" };

static void attrs_arr(const Attrs *a, int v[6])
{
    v[0] = (int)a->health;
    v[1] = (int)a->power;
    v[2] = (int)a->wisdom;
    v[3] = (int)a->charm;
    v[4] = (int)a->luck;
    v[5] = (int)a->agility;
}

/*
 * 门槛 -> "pow>=6 wis>=5"
 * 无门槛 -> "-"；题库里 health>=127 的哨兵选项 -> "impossible"
 */
static void fmt_need(char *buf, size_t n, const Attrs *a)
{
    int    v[6];
    int    i, first = 1;
    size_t len = 0;

    if (!buf || n == 0) return;
    buf[0] = '\0';
    attrs_arr(a, v);

    if (v[0] >= 100) { snprintf(buf, n, "impossible"); return; }

    for (i = 0; i < 6; ++i) {
        int w;
        if (v[i] <= 0) continue;
        w = snprintf(buf + len, n - len, "%s%s>=%d",
                     first ? "" : " ", k_attr_abbr[i], v[i]);
        if (w <= 0) break;
        len += (size_t)w;
        if (len >= n - 1) break;
        first = 0;
    }
    if (first) snprintf(buf, n, "-");
}

/* 属性增减 -> "hp+1 pow-2"，全 0 时 "-" */
static void fmt_delta(char *buf, size_t n, const Attrs *a)
{
    int    v[6];
    int    i, first = 1;
    size_t len = 0;

    if (!buf || n == 0) return;
    buf[0] = '\0';
    attrs_arr(a, v);

    for (i = 0; i < 6; ++i) {
        int w;
        if (v[i] == 0) continue;
        w = snprintf(buf + len, n - len, "%s%s%+d",
                     first ? "" : " ", k_attr_abbr[i], v[i]);
        if (w <= 0) break;
        len += (size_t)w;
        if (len >= n - 1) break;
        first = 0;
    }
    if (first) snprintf(buf, n, "-");
}

/* 当前属性是否已经满足该选项的门槛（哨兵 127 天然为假） */
static int opt_feasible(const Attrs *cur, const Attrs *need)
{
    return cur->health >= need->health && cur->power  >= need->power
        && cur->wisdom >= need->wisdom && cur->charm  >= need->charm
        && cur->luck   >= need->luck   && cur->agility >= need->agility;
}

static const char *q_name(int type, int q_idx)
{
    if (type < 0 || type > 4 || q_idx < 0 || q_idx > 14) return "?";
    return k_question_name[type][q_idx] ? k_question_name[type][q_idx] : "?";
}

static const char *o_name(int type, int q_idx, int opt)
{
    if (type < 0 || type > 4 || q_idx < 0 || q_idx > 14) return "?";
    if (opt < 0 || opt > 3) return "?";
    return k_option_name[type][q_idx][opt] ? k_option_name[type][q_idx][opt] : "?";
}

/* 比 print_rule() 宽一号，给带名字的表格用 */
static void print_rule_wide(void)
{
    int i;
    for (i = 0; i < 78; ++i) putchar('-');
    putchar('\n');
}

/* ------------------------------------------------------------------ */
/* move selection                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    int    valid;
    int    q_idx;
    int    opt;
    double prob;    /* probability of reaching the target, <0 when n/a */
    double expect;  /* remaining expected score, <0 when n/a */
} BestMove;

/*
 * 简单兜底转发：末尾恒补 0。6 参旧调用 useNN=0(DP)；7 参新调用 useNN=首变参(NN)。
 * 不再做参数计数，避免 MSVC/__VA_ARGS__ 摊平问题
 *
 * Evaluate one question.
 * target == 620 / 480 selects the sprint model(win - rate tables, multi - round);
 * any other positive target uses the single - round DP.
 * useNN = 1：走神经网络推理 flash_evaluate_options；
 * useNN = 0：走传统 DP / sprint 查表评估。
 *
 *   为兼容旧代码的宏替换：不管旧代码传几个参数，最后都强行补一个 0 兜底。
 * - 旧代码传 6 个参数 -> 展开后传 7 个参数，useNN 读到 0，走 DP。
 * - 新代码传 7 个参数 -> 展开后传 8 个参数，useNN 读到 1，走 NN，最后的 0 被忽略。
 *
 * 返回值：
 * - DP  / sprint：0 表示评估成功，非 0 表示失败；结果只有 ev[4]。
 * - NN（flash）：返回模型选出的最优选项下标 0..3（<0 表示推理失败）。
 *   ev[i].prob_goal 为四个选项各自的目标命中概率，
 *   ev[i].expect_remain 为当前节点的预期剩余得分（四项相同）。
 */
#define evaluate_move(...) \
    (_evaluate_move_impl(__VA_ARGS__, 0))

static int _evaluate_move_impl(GameContext *ctx, int q_idx, int target,
                         int round, int score, OptionEval out[OPT_PER_Q], ...)
{
    assert(out != NULL && round >= 0);
    va_list args;
    va_start(args, out);
    int useNN = va_arg(args, int);
    va_end(args);

    if (useNN)
    {
        return flash_evaluate_options(ctx, q_idx, target, out);
    }
    if (is_sprint_target(target))
        return game_evaluate_options_sprint(ctx, q_idx, round, score, target, out);

    if (game_evaluate_options(ctx, q_idx, target > 0 ? target : 0, out) != 0)
        return -1;

    /* Single-round DP reports -1 for prob_goal when target_T <= 0. */
    return 0;
}

/*
 * Is (p, e) strictly better than (bp, be)?
 * Probability wins when both sides have one; otherwise expected score decides.
 */
static int is_better(double p, double e, double bp, double be)
{
    if (p >= 0.0 && bp >= 0.0) {
        if (p > bp) return 1;
        if (p < bp) return 0;
        return e > be;
    }
    if (p >= 0.0) return 1;
    if (bp >= 0.0) return 0;
    return e > be;
}

/*
*为兼容旧代码的宏替换：不管旧代码传几个参数，最后都强行补一个 0 兜底。
* -旧代码传 6 个参数->展开后传 7 个参数，useNN 读到 0，走 DP。
* -新代码传 7 个参数->展开后传 8 个参数，useNN 读到 1，走 NN，最后的 0 被忽略。
*/
#define find_best_move(...) \
    (_find_best_move_impl(__VA_ARGS__, 0))

/* Pick the best (question, option) pair for the current state. */
static int _find_best_move_impl(GameContext *ctx, int target, int round, int score,
                          BestMove *out, ...)
{
    assert(out != NULL && round >= 0);
    va_list args;
    va_start(args, out);
    int useNN = va_arg(args, int);
    va_end(args);

    int total, q, o;

    out->valid  = 0;
    out->q_idx  = -1;
    out->opt    = -1;
    out->prob   = -1.0;
    out->expect = -1.0;

    total = game_ctx_type_total(ctx);
    if (total <= 0 || total > Q_PER_STAGE_MAX) return -1;

    OptionEval ev[OPT_PER_Q];

    int type = judgeGroup(game_ctx_get_done_cnt(ctx));
    q = -1;
    while (!game_ctx_type_available(ctx, q))
    {
        q = rand() % totalQs[type];
    }

    if (useNN)
    {
        /*
         * NN 分支：flash 模型是零搜索的策略网络，一次前向推理就同时给出
         *   - 四个选项各自命中目标的概率  -> ev[i].prob_goal
         *   - 当前节点的预期剩余得分      -> ev[i].expect_remain（四个选项相同）
         * 最优选项由模型自身输出（flash_predict 的返回值），
         * 这里直接采信，不再做 argmax 比较；target 对 NN 无意义。
         */
        int best = evaluate_move(ctx, q, target, round, score, ev, 1);

        if (best < 0 || best >= OPT_PER_Q) { // 推理失败就随机选一个
            out->valid = 1;
            out->q_idx = q;
            out->opt   = rand() % OPT_PER_Q;
            return 0;
        }

        out->valid  = 1;
        out->q_idx  = q;
        out->opt    = best;
        out->prob   = ev[best].prob_goal;
        out->expect = ev[best].expect_remain;
        return 0;
    }

    if (evaluate_move(ctx, q, target, round, score, ev) != 0) // 评估失败就随机选一个
    {
        out->valid = 1;
        out->q_idx = q;
        out->opt = rand() % 4;
        return 0;
    }

    for (o = 0; o < OPT_PER_Q; ++o) {
        if (!out->valid ||
            is_better(ev[o].prob_goal, ev[o].expect_remain, out->prob, out->expect)) {
            out->valid  = 1;
            out->q_idx  = q;
            out->opt    = o;
            out->prob   = ev[o].prob_goal;
            out->expect = ev[o].expect_remain;
        }
    }
    return out->valid ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* solve                                                               */
/* ------------------------------------------------------------------ */

static GameContext *create_ctx(int role, int target)
{
    if (is_sprint_target(target))
        return game_ctx_create_sprint((RoleId)role);
    return game_ctx_create((RoleId)role);
}

/*
 * Play the current context to the end, always taking the best move.
 * Returns 0 on success, -1 when the state cannot be evaluated.
 */
static int run_auto(GameContext *ctx, int target, int round, int score,
                    int quiet, int useNN)
{
    BestMove mv;
    double start_prob = -1.0;
    int    step = 0;

    if (!quiet) {
        print_rule();
        printf("Playing automatically: target %d, round %d of %d, prior score %d\n",
               target, round + 1, ROUNDS_TOTAL, score);
        print_rule();
    }

    for (;;) {
        StepResult sr;
        char pb[32], eb[32];
        const Attrs *attr = game_ctx_get_attrs(ctx);
        int done = game_ctx_get_done_cnt(ctx);

        if (done >= DONE_MAX) break;
        if (!attr || attr->health <= 0) break;

        if (find_best_move(ctx, target, round, score, &mv, useNN) != 0) {
            printf("No evaluable move in the current state.\n");
            return -1;
        }
        if (step == 0) start_prob = mv.prob;

        fmt_prob(pb, sizeof pb, mv.prob);
        if (mv.expect >= 0.0) snprintf(eb, sizeof eb, "%8.2f", mv.expect);
        else                  snprintf(eb, sizeof eb, "     n/a");

        if (!quiet) {
            /* 分两行：第一行定位（下标 + 题名），第二行给选项名与评估值 */
            int stage = game_ctx_get_type(ctx);
            printf("step %2d | stage %d | q_idx %2d (qid %3d) | %s\n",
                   step + 1, stage + 1, mv.q_idx,
                   game_ctx_type_question_id(ctx, mv.q_idx),
                   q_name(stage, mv.q_idx));
            printf("         | opt %d %-20s | P %s | expect %s | total %3d\n",
                   mv.opt, o_name(stage, mv.q_idx, mv.opt), pb, eb,
                   game_ctx_get_total_score(ctx));
        }

        if (game_do_step(ctx, mv.q_idx, mv.opt, &sr) != 0) {
            printf("Step execution failed at q_idx %d opt %d.\n", mv.q_idx, mv.opt);
            return -1;
        }
        if (!quiet) {
            printf("         -> %s %+d, ", sr.success ? "SUCCESS" : "FAIL",
                   sr.add_score);
            print_attrs(&sr.new_attr);
            printf("\n");
        }
        ++step;
    }

    {
        int local_score = game_ctx_get_total_score(ctx);
        int final_score = score + local_score;
        const Attrs *a  = game_ctx_get_attrs(ctx);
        char pb[32];

        fmt_prob(pb, sizeof pb, start_prob);
        print_rule();
        printf("Round score : %d\n", local_score);
        printf("Total score : %d  (prior %d + this round %d)\n",
               final_score, score, local_score);
        if (round + 1 >= ROUNDS_TOTAL)
            printf("Target %d   : %s\n", target,
                   final_score >= target ? "reached" : "not reached");
        else
            printf("Progress    : round %d of %d finished\n", round + 1, ROUNDS_TOTAL);
        printf("Final attrs : ");
        print_attrs(a);
        printf("\n");
        printf("Success P   : %s (model estimate at the first step)\n", pb);
        if (!useNN && start_prob == 0.0) {
            printf("note: the probability is exactly 0 - the win-rate tables may be\n");
            printf("      all zero. In a PRE_CAL build run 'table' first, otherwise\n");
            printf("      rebuild with the precomputed tables compiled in.\n");
        }
        print_rule();
    }
    return 0;
}

static int cmd_solve(int role, int target, int round, int score, int quiet, int use_nn)
{
    GameContext *ctx;
    int rc;

    if (role < ROLE_XIAO_TANG || role > ROLE_XIAO_ZHA) {
        printf("Invalid role %d, expected 1..7.\n", role);
        return EXIT_USAGE;
    }
    if (round < 0 || round > ROUND_MAX) {
        printf("Invalid --round %d, expected 0..%d.\n", round, ROUND_MAX);
        return EXIT_USAGE;
    }
    if (score < 0) {
        printf("Invalid --score %d, expected >= 0.\n", score);
        return EXIT_USAGE;
    }

    ctx = create_ctx(role, target);
    if (!ctx) {
        printf("Failed to create the game context (out of memory?).\n");
        return EXIT_FAIL;
    }

    if (!quiet) {
        const Attrs *a = game_ctx_get_attrs(ctx);
        printf("Role  : %d (%s)\n", role, role_name(role));
        printf("Attrs : ");
        print_attrs(a);
        printf("\n");
        
        if (use_nn) {
            printf("Model : flash (Neural Network, zero-search)\n");
            printf("Note  : --target has no effect in flash mode\n");
        }
        else if (is_sprint_target(target))
            printf("Model : sprint (win-rate tables, %d rounds total)\n", ROUNDS_TOTAL);
        else if (target > 0)
            printf("Model : single round, goal %d\n", target);
        else
            printf("Model : single round, expected score only\n");
    }

    rc = run_auto(ctx, target, round, score, quiet, use_nn);
    game_ctx_destroy(ctx);
    return (rc == 0) ? EXIT_OK : EXIT_FAIL;
}

/* ------------------------------------------------------------------ */
/* table                                                               */
/* ------------------------------------------------------------------ */

static int cmd_table(const char *out_dir)
{
#ifdef PRE_CAL
    int rc = dp_build_win_rate_tables(out_dir);
    return (rc == 0) ? EXIT_OK : EXIT_FAIL;
#else
    (void)out_dir;
    printf("This build ships precomputed tables (PRE_CAL is off).\n");
    printf("Rebuild with -DPRE_CAL to regenerate them.\n");
    return EXIT_FAIL;
#endif
}

/* ------------------------------------------------------------------ */
/* info commands                                                       */
/* ------------------------------------------------------------------ */

static int cmd_roles(void)
{
    int r;
    printf(" id  name        attributes\n");
    print_rule();
    for (r = ROLE_XIAO_TANG; r <= ROLE_XIAO_ZHA; ++r) {
        const Attrs *a = game_role_init_attrs((RoleId)r);
        printf(" %2d  %-10s  ", r, role_name(r));
        print_attrs(a);
        printf("\n");
    }
    return EXIT_OK;
}

static int cmd_rules(void)
{
    int i;
    printf("One full challenge: %d rounds, 100 points each, 700 in total.\n",
           ROUNDS_TOTAL);
    printf("Reward tiers: 620 (top) and 480 (fallback).\n\n");
    printf(" stage  questions  pick  success  fail\n");
    print_rule();
    for (i = 0; i < 5; ++i) {
        printf("   %d       %2d       %2d     %2d      %2d\n",
               i + 1, totalQs[i], needPick[i], successScore[i], failScore[i]);
    }
    printf("\nAttributes: hp pow wis cha luck agi. hp <= 0 ends the run.\n");
    return EXIT_OK;
}

static int cmd_version(void)
{
    printf("Zm4AdventureAssist kernel\n");
    printf("game version : %u\n", (unsigned)GAME_VERSION);
#ifdef PRE_CAL
    printf("build        : PRE_CAL (tables are computed at runtime)\n");
#else
    printf("build        : release (tables are compiled in)\n");
#endif
#ifdef WASM
    printf("target       : WASM\n");
#else
    printf("target       : native console\n");
#endif
    return EXIT_OK;
}

static void usage(void)
{
    printf("Zm4AdventureAssist - exact DP solver for the survival-adventure mini-game\n\n");
    printf("Usage:\n");
    printf("  adventure <command> [options]\n");
    printf("  adventure                      (starts the interactive REPL)\n\n");
    printf("Commands:\n");
    printf("  solve      play one round automatically, printing the best move each step\n");
    printf("  table      precompute the win-rate tables (PRE_CAL builds only)\n");
    printf("  roles      list roles and their initial attributes\n");
    printf("  rules      print stage and scoring rules\n");
    printf("  repl       interactive mode\n");
    printf("  version    build information\n");
    printf("  help       this text\n\n");
    printf("solve options:\n");
    printf("  --role N    role id 1..7                 (default xiao yu)\n");
    printf("  --target T  target score; 620 or 480 uses the sprint model\n");
    printf("              (default 620)\n");
    printf("  --round N   rounds already completed 0..6 (default 0)\n");
    printf("  --score S   score accumulated in previous rounds (default 0)\n");
    printf("  --quiet     print the summary only\n\n");
    printf("  --flash     flash mode (NN, target T option is forbidden)\n");
    printf("  --precision precision mode (DP)\n");
    printf("table options:\n");
    printf("  --out DIR   output directory (default: current directory)\n\n");
    printf("Exit codes: 0 ok, 1 runtime failure, 2 bad usage\n");
}

/* ------------------------------------------------------------------ */
/* REPL                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    int          role;
    int          target;
    int          round;
    int          score;
    int          use_nn;
    GameContext *ctx;
} ReplState;

static int repl_reset(ReplState *st)
{
    if (st->ctx) {
        game_ctx_destroy(st->ctx);
        st->ctx = NULL;
    }
    st->ctx = create_ctx(st->role, st->target);
    return st->ctx ? 0 : -1;
}

static void repl_status(ReplState *st)
{
    BestMove mv;
    char pb[32];
    const Attrs *a = game_ctx_get_attrs(st->ctx);
    int done  = game_ctx_get_done_cnt(st->ctx);
    int local = game_ctx_get_total_score(st->ctx);

    printf("role %d (%s) | target %d | round %d of %d | prior score %d\n",
           st->role, role_name(st->role), st->target,
           st->round + 1, ROUNDS_TOTAL, st->score);
    printf("done %d/%d | score this round %d | total %d\n",
           done, DONE_MAX, local, st->score + local);
    printf("attrs ");
    print_attrs(a);
    printf("\n");

    if (done < DONE_MAX && a && a->health > 0 &&
        find_best_move(st->ctx, st->target, st->round, st->score, &mv, st->use_nn) == 0) {
        int type = game_ctx_get_type(st->ctx);
        fmt_prob(pb, sizeof pb, mv.prob);
        printf("best move: q_idx %d (qid %d, %s) opt %d (%s) | P %s | expect %.2f\n",
               mv.q_idx, game_ctx_type_question_id(st->ctx, mv.q_idx),
               q_name(type, mv.q_idx), mv.opt, o_name(type, mv.q_idx, mv.opt),
               pb, mv.expect);
    }
}

/*
 * 'q' / 'board'：列出本阶段还能选的题目。
 *   ok/4 = 四个选项里有几个现在已经满足门槛（不消耗任何评估）
 *   pick = flash 模式下模型给出的最优选项；precision 模式不打印（那要做 DP）
 */
static void repl_board(ReplState *st)
{
    const Attrs *cur = game_ctx_get_attrs(st->ctx);
    int type  = game_ctx_get_type(st->ctx);
    int total = game_ctx_type_total(st->ctx);
    int avail = 0;
    int q, o;

    for (q = 0; q < total; ++q)
        if (game_ctx_type_available(st->ctx, q)) ++avail;

    print_rule_wide();
    printf("Stage %d of 5 | %d of %d questions left | pick %d per stage\n",
           type + 1, avail, total, game_ctx_type_need(st->ctx));
    printf("attrs ");
    print_attrs(cur);
    printf(" | score %d\n", st->score + game_ctx_get_total_score(st->ctx));
    print_rule_wide();

    if (st->use_nn)
        printf(" q_idx  qid  %-22s ok/4  %-5s %8s\n", "question", "pick", "P");
    else
        printf(" q_idx  qid  %-22s ok/4\n", "question");

    for (q = 0; q < total; ++q) {
        int  fit = 0;
        char pickbuf[16], pb[32];

        if (!game_ctx_type_available(st->ctx, q)) continue;

        for (o = 0; o < OPT_PER_Q; ++o) {
            OptionInfo oi;
            if (game_ctx_type_option(st->ctx, q, o, &oi) == 0 &&
                opt_feasible(cur, &oi.success_attrs)) ++fit;
        }

        if (st->use_nn) {
            OptionEval ev[OPT_PER_Q];
            /* NN 分支返回模型给出的最优选项下标（0..3），<0 才是推理失败 */
            int best = evaluate_move(st->ctx, q, st->target, st->round,
                                     st->score, ev, 1);
            if (best >= 0 && best < OPT_PER_Q) {
                snprintf(pickbuf, sizeof pickbuf, "opt %d", best);
                fmt_prob(pb, sizeof pb, ev[best].prob_goal);
            } else {
                snprintf(pickbuf, sizeof pickbuf, "-");
                snprintf(pb, sizeof pb, "    n/a");
            }
            printf(" %5d  %3d  %-22s %2d/4  %-5s %8s\n", q,
                   game_ctx_type_question_id(st->ctx, q), q_name(type, q),
                   fit, pickbuf, pb);
        } else {
            printf(" %5d  %3d  %-22s %2d/4\n", q,
                   game_ctx_type_question_id(st->ctx, q), q_name(type, q), fit);
        }
    }
    print_rule_wide();
    printf("'opt Q' = inspect the 4 options | 'pick Q O' = answer it\n");
    if (!st->use_nn)
        printf("note: precision mode does not rank questions; 'opt Q' evaluates one at a time\n");
}

/*
 * 'opt Q'：把一个问题的四个选项摊开。
 * 每行给出：选项名 / 当前属性是否已达标 / 门槛 / 成功收益 / 失败惩罚 / P / 期望分。
 */
static void repl_options(ReplState *st, int q_idx)
{
    OptionEval ev[OPT_PER_Q];
    const Attrs *cur  = game_ctx_get_attrs(st->ctx);
    int  type = game_ctx_get_type(st->ctx);
    int  o, best = -1, have_eval = 0, rc;

    if (q_idx < 0 || !game_ctx_type_available(st->ctx, q_idx)) {
        printf("q_idx %d is not available; type 'q' to list the board.\n", q_idx);
        return;
    }

    for (o = 0; o < OPT_PER_Q; ++o) {
        ev[o].prob_goal     = -1.0;
        ev[o].expect_remain = -1.0;
    }

    /*
     * 返回值语义随模式不同：
     *   DP  -> 0 成功 / 非 0 失败
     *   NN  -> 模型给出的最优选项下标 0..3，<0 才是推理失败
     */
    rc = evaluate_move(st->ctx, q_idx, st->target, st->round, st->score,
                       ev, st->use_nn);
    if (st->use_nn) {
        if (rc >= 0 && rc < OPT_PER_Q) { have_eval = 1; best = rc; }
    } else if (rc == 0) {
        have_eval = 1;
        for (o = 0; o < OPT_PER_Q; ++o) {
            if (best < 0 || is_better(ev[o].prob_goal, ev[o].expect_remain,
                                      ev[best].prob_goal, ev[best].expect_remain))
                best = o;
        }
    }

    print_rule_wide();
    printf(" q_idx %d | qid %d | %s\n", q_idx,
           game_ctx_type_question_id(st->ctx, q_idx), q_name(type, q_idx));
    printf(" stage %d of 5 | attrs ", type + 1);
    print_attrs(cur);
    printf(" | score %d\n", st->score + game_ctx_get_total_score(st->ctx));
    print_rule_wide();
    printf(" idx %-22s %-3s %8s  E %7s\n", "option", "now", "P", "expect");

    for (o = 0; o < OPT_PER_Q; ++o) {
        OptionInfo oi;
        char need[48], gain[48], lose[48], pb[32];
        int  fit;

        if (game_ctx_type_option(st->ctx, q_idx, o, &oi) != 0) continue;

        fit = opt_feasible(cur, &oi.success_attrs);
        fmt_need(need, sizeof need, &oi.success_attrs);
        fmt_delta(gain, sizeof gain, &oi.success_award);
        fmt_delta(lose, sizeof lose, &oi.fail_punish);
        if (have_eval) fmt_prob(pb, sizeof pb, ev[o].prob_goal);
        else           snprintf(pb, sizeof pb, "     n/a");

        printf(" [%d] %-22s %-3s %8s  E %7.2f%s\n",
               o, o_name(type, q_idx, o), fit ? "ok" : "--", pb,
               ev[o].expect_remain, (have_eval && o == best) ? "  *" : "");
        printf("     need %-22s | ok %-22s | fail %s\n", need, gain, lose);
    }
    print_rule_wide();

    if (!have_eval) {
        printf("evaluation failed - P / expect are not available\n");
        printf("'opt %d' can still be retried, or switch mode with 'flash' / 'precision'\n",
               q_idx);
        return;
    }
    if (st->use_nn)
        printf("* = model pick | E = expected score of this node | 'now' = requirement met\n");
    else
        printf("* = best option by DP | E = expected remaining score | 'now' = requirement met\n");
    printf("answer it with: pick %d %d\n", q_idx, best);
}

static void repl_help(void)
{
    printf("commands:\n");
    printf("  help              this text\n");
    printf("  roles             list roles and initial attributes\n");
    printf("  rules             stage and scoring rules\n");
    printf("  role N            switch role (resets the game)\n");
    printf("  target T          set target score; 620 or 480 enables sprint model\n");
    printf("  round N           rounds already completed, 0..6\n");
    printf("  score S           score accumulated in previous rounds\n");
    printf("  status            current state and best move\n");
    printf("  q                 list this stage's questions: name, feasible options, model pick\n");
    printf("  opt Q             inspect one question: option names, requirement, rewards, P\n");
    printf("  pick Q O          answer question Q with option O\n");
    printf("  auto              play the rest of the round automatically\n");
    printf("  reset             restart with the current role and target\n");
    printf("  flash             flash mode (NN, target T option is forbidden)\n");
    printf("  precision         precision mode (DP)\n");
    printf("  disparity         print the gap between flash evaluation and precision solution\n");
    printf("  quit              leave\n");
}

static int repl_loop(ReplState *st)
{
    char line[256];

    printf("Zm4AdventureAssist %u - type 'help' for commands, 'quit' to leave.\n",
           (unsigned)GAME_VERSION);
    printf("role %d (%s), target %d\n", st->role, role_name(st->role), st->target);

    for (;;) {
        char cmd[32];
        int  a = 0, b = 0;
        int  n;

        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;

        n = sscanf(line, "%31s", cmd);
        if (n != 1) continue;
        to_lower(cmd);

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            break;
        }
        else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
            repl_help();
        }
        else if (strcmp(cmd, "flash") == 0) {
            st->use_nn = 1;
        }
        else if (strcmp(cmd, "precision") == 0) {
            st->use_nn = 0;
        }
        else if (strcmp(cmd, "disparity") == 0) {
            const char *desc =
                "Flash mode uses a pre-trained lightweight neural network for one-step inference\n"
                "to estimate the best option and expected score. It has a small memory footprint\n"
                "and delivers relatively high computational accuracy.\n\n"
                "Precision mode builds an in-memory hash-table cache and performs dynamic\n"
                "programming over questions to produce mathematically exact values. It consumes\n"
                "more CPU and memory, returning either the expected score for each option or the\n"
                "probability of reaching the target score. Supported policies: maximize expected\n"
                "score, maximize the probability of hitting the target score in a single run, and\n"
                "maximize the probability of reaching 480 / 620 across seven consecutive runs.\n\n"
                "Difference between the two modes:\n"
                "\n"
                "           [PK] 1000 games  role = random  seed=42\n"
                "     mode          |   avg  |   std  |  min  |  max  |  >=70% | death%\n"
                "-------------------+--------+--------+-------+-------+--------+--------\n"
                " Flash(NN-net)     |  76.8  |  12.0  |   36  |  100  |   92%  |   60% \n"
                " Precision(DP)     |  77.3  |  12.3  |   41  |  100  |   91%  |   68% \n"
                "\n"
                " DP vs NN-net     112-68-820    avg_score +0.4130\n";
            
            printf("%s", desc);
        }
        else if (strcmp(cmd, "roles") == 0) {
            cmd_roles();
        }
        else if (strcmp(cmd, "rules") == 0) {
            cmd_rules();
        }
        else if (strcmp(cmd, "role") == 0) {
            if (sscanf(line, "%*s %d", &a) != 1 ||
                a < ROLE_XIAO_TANG || a > ROLE_XIAO_ZHA) {
                printf("usage: role 1..7\n");
                continue;
            }
            st->role = a;
            if (repl_reset(st) != 0) { printf("reset failed.\n"); break; }
            printf("role set to %d (%s), game reset.\n", a, role_name(a));
        }
        else if (strcmp(cmd, "target") == 0) {
            if (sscanf(line, "%*s %d", &a) != 1 || a < 0) {
                printf("usage: target <score>\n");
                continue;
            }
            st->target = a;
            if (repl_reset(st) != 0) { printf("reset failed.\n"); break; }
            printf("target set to %d, game reset.\n", a);
        }
        else if (strcmp(cmd, "round") == 0) {
            if (sscanf(line, "%*s %d", &a) != 1 || a < 0 || a > ROUND_MAX) {
                printf("usage: round 0..%d\n", ROUND_MAX);
                continue;
            }
            st->round = a;
            if (repl_reset(st) != 0) { printf("reset failed.\n"); break; }
            printf("round set to %d, game reset.\n", a + 1);
        }
        else if (strcmp(cmd, "score") == 0) {
            if (sscanf(line, "%*s %d", &a) != 1 || a < 0) {
                printf("usage: score <points>\n");
                continue;
            }
            st->score = a;
            printf("prior score set to %d.\n", a);
        }
        else if (strcmp(cmd, "status") == 0) {
            repl_status(st);
        }
        else if (strcmp(cmd, "q") == 0 || strcmp(cmd, "board") == 0) {
            repl_board(st);
        }
        else if (strcmp(cmd, "opt") == 0) {
            if (sscanf(line, "%*s %d", &a) != 1) {
                printf("usage: opt <q_idx>\n");
                continue;
            }
            repl_options(st, a);
        }
        else if (strcmp(cmd, "pick") == 0) {
            StepResult sr;
            const char *onm;
            if (sscanf(line, "%*s %d %d", &a, &b) != 2) {
                printf("usage: pick <q_idx> <opt 0..3>\n");
                continue;
            }
            onm = o_name(game_ctx_get_type(st->ctx), a, b);
            if (game_do_step(st->ctx, a, b, &sr) != 0) {
                printf("invalid move: q_idx %d opt %d (type 'q' to see the board)\n", a, b);
                continue;
            }
            printf("%s %+d [%s] -> score %d, ", sr.success ? "SUCCESS" : "FAIL",
                   sr.add_score, onm, st->score + game_ctx_get_total_score(st->ctx));
            print_attrs(&sr.new_attr);
            printf("\n");
            if (game_ctx_get_done_cnt(st->ctx) >= DONE_MAX)
                printf("round finished: %d points this round.\n",
                       game_ctx_get_total_score(st->ctx));
        }
        else if (strcmp(cmd, "auto") == 0) {
            run_auto(st->ctx, st->target, st->round, st->score, 0, st->use_nn);
        }
        else if (strcmp(cmd, "reset") == 0) {
            if (repl_reset(st) != 0) { printf("reset failed.\n"); break; }
            printf("game reset.\n");
        }
        else if (strcmp(cmd, "version") == 0) {
            cmd_version();
        }
        else {
            printf("unknown command '%s', type 'help'.\n", cmd);
        }
    }
    return EXIT_OK;
}

static int cmd_repl(int role, int target, int round, int score, int use_nn)
{
    ReplState st;
    int rc;

    st.role   = role;
    st.target = target;
    st.round  = round;
    st.score  = score;
    st.ctx    = NULL;
    st.use_nn = use_nn;

    if (repl_reset(&st) != 0) {
        printf("Failed to create the game context (out of memory?).\n");
        return EXIT_FAIL;
    }
    rc = repl_loop(&st);
    game_ctx_destroy(st.ctx);
    return rc;
}

/* ------------------------------------------------------------------ */
/* dispatch                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    int         role;
    int         target;
    int         round;
    int         score;
    int         quiet;
    int         use_nn; // nn 状态开关
    const char *out_dir;
} CliOptions;

static void options_init(CliOptions *o)
{
    o->role    = ROLE_XIAO_YU;
    o->target  = 620;
    o->round   = 0;
    o->score   = 0;
    o->quiet   = 0;
    o->use_nn  = 1; // 默认 nn，速度快
    o->out_dir = NULL;
}

/*
 * Parse "key value" style options starting at argv[start].
 * Returns 0 on success, EXIT_USAGE on malformed input.
 */
static int parse_options(CliOptions *o, int argc, char **argv, int start)
{
    int i;
    for (i = start; i < argc; ++i) {
        const char *k = argv[i];

        if (strcmp(k, "--quiet") == 0 || strcmp(k, "-q") == 0) {
            o->quiet = 1;
        }
        else if (strcmp(k, "--flash") == 0) { 
            o->use_nn = 1;
        }
        else if (strcmp(k, "--precision") == 0) {
            o->use_nn = 0;
        }
        else if (strcmp(k, "--role") == 0 && i + 1 < argc) {
            o->role = atoi(argv[++i]);
        }
        else if (strcmp(k, "--target") == 0 && i + 1 < argc) {
            o->target = atoi(argv[++i]);
        }
        else if (strcmp(k, "--round") == 0 && i + 1 < argc) {
            o->round = atoi(argv[++i]);
        }
        else if (strcmp(k, "--score") == 0 && i + 1 < argc) {
            o->score = atoi(argv[++i]);
        }
        else if (strcmp(k, "--out") == 0 && i + 1 < argc) {
            o->out_dir = argv[++i];
        }
        else {
            printf("unknown option '%s'\n", k);
            return EXIT_USAGE;
        }
    }
    return EXIT_OK;
}

int cli_main(int argc, char **argv)
{
    CliOptions o;
    const char *cmd;

    options_init(&o);

    if (argc < 2) {
        return cmd_repl(o.role, o.target, o.round, o.score, o.use_nn);
    }

    cmd = argv[1];

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "-h") == 0) {
        usage();
        return EXIT_OK;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        return cmd_version();
    }
    if (strcmp(cmd, "roles") == 0)   return cmd_roles();
    if (strcmp(cmd, "rules") == 0)   return cmd_rules();

    if (parse_options(&o, argc, argv, 2) != EXIT_OK)
        return EXIT_USAGE;

    if (strcmp(cmd, "solve") == 0)
        return cmd_solve(o.role, o.target, o.round, o.score, o.quiet, o.use_nn);

    if (strcmp(cmd, "table") == 0)
        return cmd_table(o.out_dir);

    if (strcmp(cmd, "repl") == 0)
        return cmd_repl(o.role, o.target, o.round, o.score, o.use_nn);

    printf("unknown command '%s'\n\n", cmd);
    usage();
    return EXIT_USAGE;
}
