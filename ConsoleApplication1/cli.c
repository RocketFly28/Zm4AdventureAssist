#define _CRT_SECURE_NO_WARNINGS

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
#include "Adventure.h"
#include "dpCal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
 * Evaluate one question. `target` == 620 / 480 selects the sprint model
 * (win-rate tables, multi-round); anything else uses the single-round DP.
 */
static int evaluate_move(GameContext *ctx, int q_idx, int target,
                         int round, int score, OptionEval out[OPT_PER_Q])
{
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

/* Pick the best (question, option) pair for the current state. */
static int find_best_move(GameContext *ctx, int target, int round, int score,
                          BestMove *out)
{
    int total, q, o;

    out->valid  = 0;
    out->q_idx  = -1;
    out->opt    = -1;
    out->prob   = -1.0;
    out->expect = -1.0;

    total = game_ctx_type_total(ctx);
    if (total <= 0 || total > Q_PER_STAGE_MAX) return -1;

    for (q = 0; q < total; ++q) {
        OptionEval ev[OPT_PER_Q];

        if (!game_ctx_type_available(ctx, q)) continue;
        if (evaluate_move(ctx, q, target, round, score, ev) != 0) continue;

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
                    int quiet)
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

        if (find_best_move(ctx, target, round, score, &mv) != 0) {
            printf("No evaluable move in the current state.\n");
            return -1;
        }
        if (step == 0) start_prob = mv.prob;

        fmt_prob(pb, sizeof pb, mv.prob);
        if (mv.expect >= 0.0) snprintf(eb, sizeof eb, "%8.2f", mv.expect);
        else                  snprintf(eb, sizeof eb, "     n/a");

        if (!quiet) {
            printf("step %2d | stage %d | q_idx %2d (qid %3d) | opt %d | P %s | expect %s | total %3d\n",
                   step + 1, game_ctx_get_type(ctx) + 1, mv.q_idx,
                   game_ctx_type_question_id(ctx, mv.q_idx), mv.opt,
                   pb, eb, game_ctx_get_total_score(ctx));
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
        printf("Success P   : %s (estimated from the first step)\n", pb);
        if (start_prob == 0.0) {
            printf("note: the probability is exactly 0 - the win-rate tables may be\n");
            printf("      all zero. In a PRE_CAL build run 'table' first, otherwise\n");
            printf("      rebuild with the precomputed tables compiled in.\n");
        }
        print_rule();
    }
    return 0;
}

static int cmd_solve(int role, int target, int round, int score, int quiet)
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
        if (is_sprint_target(target))
            printf("Model : sprint (win-rate tables, %d rounds total)\n", ROUNDS_TOTAL);
        else if (target > 0)
            printf("Model : single round, goal %d\n", target);
        else
            printf("Model : single round, expected score only\n");
    }

    rc = run_auto(ctx, target, round, score, quiet);
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
    printf("  --role N    role id 1..7                 (default 3)\n");
    printf("  --target T  target score; 620 or 480 uses the sprint model\n");
    printf("              (default 620)\n");
    printf("  --round N   rounds already completed 0..6 (default 0)\n");
    printf("  --score S   score accumulated in previous rounds (default 0)\n");
    printf("  --quiet     print the summary only\n\n");
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
        find_best_move(st->ctx, st->target, st->round, st->score, &mv) == 0) {
        fmt_prob(pb, sizeof pb, mv.prob);
        printf("best move: q_idx %d (qid %d) opt %d | P %s | expect %.2f\n",
               mv.q_idx, game_ctx_type_question_id(st->ctx, mv.q_idx),
               mv.opt, pb, mv.expect);
    }
}

static void repl_board(ReplState *st)
{
    int total = game_ctx_type_total(st->ctx);
    int q;
    printf("stage %d: pick %d of the following\n",
           game_ctx_get_type(st->ctx) + 1, game_ctx_type_need(st->ctx));
    for (q = 0; q < total; ++q) {
        if (!game_ctx_type_available(st->ctx, q)) continue;
        printf("  q_idx %2d  qid %3d\n", q, game_ctx_type_question_id(st->ctx, q));
    }
}

static void repl_options(ReplState *st, int q_idx)
{
    OptionEval ev[OPT_PER_Q];
    int o;

    if (q_idx < 0 || !game_ctx_type_available(st->ctx, q_idx)) {
        printf("q_idx %d is not available.\n", q_idx);
        return;
    }
    if (evaluate_move(st->ctx, q_idx, st->target, st->round, st->score, ev) != 0) {
        printf("evaluation failed for q_idx %d.\n", q_idx);
        return;
    }
    for (o = 0; o < OPT_PER_Q; ++o) {
        OptionInfo oi;
        char pb[32];
        if (game_ctx_type_option(st->ctx, q_idx, o, &oi) != 0) continue;
        fmt_prob(pb, sizeof pb, ev[o].prob_goal);
        printf("  opt %d | need ", o);
        print_attrs(&oi.success_attrs);
        printf(" | P %s | expect %8.2f\n", pb, ev[o].expect_remain);
    }
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
    printf("  q                 list available questions\n");
    printf("  opt Q             show the four options of question Q\n");
    printf("  pick Q O          answer question Q with option O\n");
    printf("  auto              play the rest of the round automatically\n");
    printf("  reset             restart with the current role and target\n");
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
            if (sscanf(line, "%*s %d %d", &a, &b) != 2) {
                printf("usage: pick <q_idx> <opt 0..3>\n");
                continue;
            }
            if (game_do_step(st->ctx, a, b, &sr) != 0) {
                printf("invalid move: q_idx %d opt %d\n", a, b);
                continue;
            }
            printf("%s %+d, score %d, ", sr.success ? "SUCCESS" : "FAIL",
                   sr.add_score, st->score + game_ctx_get_total_score(st->ctx));
            print_attrs(&sr.new_attr);
            printf("\n");
            if (game_ctx_get_done_cnt(st->ctx) >= DONE_MAX)
                printf("round finished: %d points this round.\n",
                       game_ctx_get_total_score(st->ctx));
        }
        else if (strcmp(cmd, "auto") == 0) {
            run_auto(st->ctx, st->target, st->round, st->score, 0);
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

static int cmd_repl(int role, int target, int round, int score)
{
    ReplState st;
    int rc;

    st.role   = role;
    st.target = target;
    st.round  = round;
    st.score  = score;
    st.ctx    = NULL;

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
    const char *out_dir;
} CliOptions;

static void options_init(CliOptions *o)
{
    o->role    = ROLE_XIAO_ZHU;
    o->target  = 620;
    o->round   = 0;
    o->score   = 0;
    o->quiet   = 0;
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
        return cmd_repl(o.role, o.target, o.round, o.score);
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
        return cmd_solve(o.role, o.target, o.round, o.score, o.quiet);

    if (strcmp(cmd, "table") == 0)
        return cmd_table(o.out_dir);

    if (strcmp(cmd, "repl") == 0)
        return cmd_repl(o.role, o.target, o.round, o.score);

    printf("unknown command '%s'\n\n", cmd);
    usage();
    return EXIT_USAGE;
}
