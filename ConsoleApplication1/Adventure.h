#ifndef ADVENTURE_H
#define ADVENTURE_H

#include "common.h"
#include <stdint.h>
#include "hash.h"   /* MemoHash / HashEntry / KEY_IS_USED */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 内核内部：规则常量、纯工具函数与 key 编码
 *
 * 【为什么放在这里】dpMaxAverage / dpGoalProb 已拆分到 dpCal.c，
 * 它们需要访问题型分组、题库、计分常量与若干纯函数；这些定义留在
 * Adventure.c，但把声明提到头文件后即可跨文件复用。
 * 均为只读数据或纯函数，可被多局安全共享。
 * key 编码相关的 PWCLA_MAX / KEY_IS_USED 见 hash.h。
 *
 * 【内核内部 bool】沿用历史定义 int 为 bool（0=FALSE, 1=TRUE）。
 * 若 bool 已被 <stdbool.h> 等定义过（wasm 侧经 emscripten.h 间接引入），
 * 则直接复用，避免 "cannot combine with previous declaration"。
 * ------------------------------------------------------------------------- */
#ifndef bool
#ifndef __cplusplus
    typedef int bool;          /* 内核内部布尔：0=FALSE, 1=TRUE（C++ 下用内建 bool） */
#endif
#endif
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
/* ---------------------------------------------------------------------------
 * 六维属性（int8_t：奖惩存在负值；127 是"条件不可能达成"的哨兵）
 * ------------------------------------------------------------------------- */
typedef struct {
    int8_t health;   /* 体力，为 0 立即失败 */
    int8_t power;    /* 力量 */
    int8_t wisdom;   /* 智慧 */
    int8_t charm;    /* 魅力 */
    int8_t luck;     /* 幸运 */
    int8_t agility;  /* 敏捷 */
} Attrs;

/* ---------------------------------------------------------------------------
 * 角色编号（对应 data.json 的 init_attrs）
 * ------------------------------------------------------------------------- */
typedef enum {
    ROLE_XIAO_TANG = 1,
    ROLE_XIAO_HOU  = 2,
    ROLE_XIAO_ZHU  = 3,
    ROLE_XIAO_SHA  = 4,
    ROLE_XIAO_BAI  = 5,
    ROLE_XIAO_YU   = 6,
    ROLE_XIAO_ZHA  = 7,
    ROLE_USER_DEFINE = 8,
} RoleId;

/* 不透明句柄：一局游戏的全部可变状态（memo、缓存、进度、调试统计） */
typedef struct GameContext GameContext;

/* ---------------------------------------------------------------------------
 * 局面快照：用于从任意进度"续档"创建一局（game_ctx_create_form）
 *
 * 【为什么不做成不透明类型】该结构体要由 JS 侧在 wasm 堆上填充后传入，
 * 因此必须在头文件里给出完整定义，且全部成员用定宽类型，保证内存布局唯一。
 *
 * 【注意】原实现里 qid_selected 声明为 bool[15]，而 Adventure.c 中有
 * `typedef int bool;`，即 bool 实为 int(4 字节)。这里改成 int32_t 语义等价，
 * 但布局对所有平台确定，避免 JS 按 1 字节写入导致错位。
 *
 * 32 位目标（wasm32）下 sizeof(GameSnapshot) = 76，布局如下（小端）：
 *   offset  0 .. 59 : qid_selected[15]，每个元素 0/1，非 0 视为"该题已选"
 *   offset 60 .. 65 : attrs（health/power/wisdom/charm/luck/agility，各 1 字节）
 *   offset      66  : done_cnt（0~9）
 *   offset  67..67  : 填充
 *   offset  68..71  : score（int32）
 *   offset  72..75  : ctx，目标 GameContext 指针
 *                     create_form 时忽略；modify 时必填，为 0 则视为非法
 * JS 侧请勿硬编码 76，调用 wasm_game_snapshot_size() 取实际值。
 * 注意：ctx 是指针，在 64 位宿主上 sizeof 会是 80，本结构体的消费方是
 * wasm32，上述偏移量仅对 32 位目标成立。
 * ------------------------------------------------------------------------- */
struct GameSnapshot {
    int32_t qid_selected[15];
    Attrs   attrs;
    uint8_t done_cnt;
    int32_t score;
    GameContext *ctx;  /* 目标上下文；仅 game_ctx_modify 使用 */
};
typedef struct GameSnapshot GameSnapshot;

/* ============================ 生命周期 ============================ */
/* 创建一局（自动加载对应角色的磁盘缓存）；失败返回 NULL */
GameContext* game_ctx_create(RoleId role_id);
GameContext* game_ctx_create_form(GameSnapshot *s);
GameContext *game_ctx_create_sprint(RoleId role_id);
GameContext *game_ctx_create_sprint_form(GameSnapshot *s);

void change_game_ctx_mode(GameContext *ctx, bool n_exp_ready, bool n_prob_ready, bool n_480_ready, bool n_620_ready);

/* 在快照 s 所关联的已有 GameContext 上原地改写局面；成功返回 0，失败返回 -1 */
int          game_ctx_modify(GameSnapshot *s);
/* 销毁一局：脏缓存落盘，释放全部内存 */
void         game_ctx_destroy(GameContext* ctx);

/* ============================ 只读访问 ============================ */
const Attrs* game_ctx_get_attrs(const GameContext* ctx);   /* 当前属性 */
int          game_ctx_get_done_cnt(const GameContext* ctx);/* 已完成题数 0~10 */
int          game_ctx_get_total_score(const GameContext* ctx);/* 累计得分 */
int          game_ctx_get_type(const GameContext* ctx);    /* 当前题型 0~4 */
uint16_t     game_ctx_get_mask(const GameContext* ctx);    /* 组内掩码 */

/* ===================== 当前题型题库查询（供前端渲染） ===================== */
int game_ctx_find_qidx_by_id(const GameContext *ctx, int question_id); /* 通过题目查询qidx */
int game_ctx_type_total(const GameContext* ctx);        /* 本组题目总数 */
int game_ctx_type_need(const GameContext* ctx);         /* 本组需完成的题数 */
int game_ctx_type_available(const GameContext* ctx, int q_idx); /* 是否可选 */
int game_ctx_type_question_id(const GameContext* ctx, int q_idx); /* 题目ID，非法返回 -1 */

typedef struct {
    Attrs success_attrs; /* 判定门槛 */
    Attrs success_award; /* 成功增减 */
    Attrs fail_punish;   /* 失败增减 */
} OptionInfo;
/* 取某题某选项的规则数据；非法返回 -1，成功返回 0 */
int game_ctx_type_option(const GameContext* ctx, int q_idx, int opt, OptionInfo* out);

/* ============================ 执行一步 ============================ */
typedef struct {
    int success;     /* 判定是否成功 */
    int add_score;   /* 本步得分 */
    Attrs new_attr;  /* 执行后属性 */
    int game_over;   /* 是否已死亡或答完 10 题 */
} StepResult;
/* 选择本组内题目 q_idx(0~组内)、选项 opt(0~3)，推进状态；成功返回 0 */
int game_do_step(GameContext* ctx, int q_idx, int opt, StepResult* out);

/* ============================ DP 查询 ============================ */
typedef struct {
    double expect_remain; /* 选该选项后的"剩余期望得分" */
    double prob_goal;     /* 选该选项后的"达到目标 T 的概率"，未启用时为 -1 */
} OptionEval;
/* 评估当前状态下某道题的 4 个选项；target_T<=0 时不计算达标概率；成功返回 0 */
int game_evaluate_options(GameContext* ctx, int q_idx, int target_T, OptionEval out[4]);

int game_evaluate_options_sprint(GameContext *ctx, int q_idx, int completed_rounds, int total_score, int target, OptionEval *out4);

/* 从当前状态出发的剩余期望得分；缓存未就绪返回 -1.0 */
double game_query_expect(GameContext* ctx);
/* 从当前状态出发，达到目标总分 T 的概率；缓存未就绪返回 -1.0 */
double game_query_goal_prob(GameContext* ctx, int T);

/* 角色初始属性 */
const Attrs* game_role_init_attrs(RoleId role_id);

/*============================= 哈希表 ============================== */
/* MemoHash / HashEntry 的完整定义在 hash.h（避免与 hash.h 的 typedef 冲突）。 */
typedef struct Question Question;
typedef struct StateVec StateVec;
typedef struct Option Option;

extern const int totalQs[5];           /* 各题型题目总数 */
extern const int needPick[5];          /* 各题型需要选定的题目数 */
extern const int successScore[5];      /* 各题型成功得分 */
extern const int failScore[5];         /* 各题型失败得分 */
extern const int maxRemainingScore[11];/* 剩余全成功时的最高分，用于剪枝 */

/* 属性增减：pwcla 下限钳位 0；health 不钳位允许负数 */
Attrs apply_attr(const Attrs old, const Attrs delta);
/* done_cnt -> 题型阶段 0~4；done_cnt==10 返回 -1 */
int judgeGroup(int done_cnt);
/* 本组内题目下标 -> 对应位的 bitmask */
uint16_t convert_group_mask(int q_idx);
/* 统计 bitmask 中置位的数量 */
int mask_count(uint16_t mask);
/* 阶段编号 -> 题库数组首地址；非法 type 返回 NULL */
const Question* question_db_for_type(int type);

/* 打包 group_mask + 属性 + done_cnt + r 成 uint64_t flat_key；
 * health<=0 返回 UINT64_MAX 代表无效死亡状态。
 * bit分配：[0-15]mask [16-19]health-1 [20-23]power [24-27]wisdom
 *          [28-31]charm [32-35]luck [36-39]agility [40-43]done [44-51]r */
uint64_t encode_flat_key(uint16_t group_mask, const Attrs raw_attr,
                         uint8_t done_cnt, uint8_t remain_aim_score);

/* 题目内部结构（dpCal.c 需要按 options[] 取规则数据） */
struct Option {
    Attrs successAttrs;  /* 成功判定门槛 */
    Attrs successAward;  /* 成功奖励增减 */
    Attrs failPunish;    /* 失败惩罚增减 */
};
typedef struct Option Option;

struct Question {
    uint16_t id;         /* 题目 id（101/102/.../505，百位即题型） */
    uint8_t  type;       /* 题型 1~5 */
    struct Option options[4]; /* 本题 4 个选项 */
};

#define OPT_PER_QUESTION 4U

/* 剪枝用状态向量：六维属性 + 是否成功标记 */
struct StateVec {
    int h, p, w, c, l, a, sc;
};

/* 选项剪枝：删掉被支配或必死且非最后题的选项。改动 vecs/keep，无外部副作用 */
void prune_options(const Question *q, const Attrs raw_attr,
                   int done_cnt, StateVec vecs[4], bool keep[4]);

/* key 编码用的属性上限与哨兵 */
#define PWCLA_MAX       12
#define HEALTH_MAX      10
#define NO_REMAIN_AIM_SCORE 0

/* ============================ 调试统计 ============================ */
/* 记录 DP 子问题 (mask, attr, done_cnt, r) 的共享结果数，供观察缓存规模 */
# ifdef DEBUG
typedef struct {
    int memo_hits;
    int memo_misses;
} DebugStats;
/* 返回 ctx 内部统计的只读指针；ctx 为 NULL 返回 NULL */
const DebugStats* game_ctx_get_stats(const GameContext* ctx);
#endif

#ifdef __cplusplus
}
#endif
#endif /* ADVENTURE_H */
