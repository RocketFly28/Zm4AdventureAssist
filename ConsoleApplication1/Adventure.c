#define _CRT_SECURE_NO_WARNINGS

#include "Adventure.h"
#include "hash.h"
#include "dpCal.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>   /* calloc / free */
#include <string.h>   /* snprintf */

#ifdef DEBUG
#include <assert.h>
#endif
/* ===========================================================================
 * 本文件为"生存大冒险"的纯计算内核。
 *
 *   - 一切可变状态都封装在 GameContext 里，由 game_ctx_create / game_ctx_destroy
 *     管理；DP 函数只通过指针参数访问 memo，不碰任何全局变量，因此可重入、
 *     支持同一进程同时跑多局（后端多会话 / WASM 均可）。
 *   - 题库常量、规则数组为 static const 只读数据，可被多局安全共享，不需要改动。
 *   - 控制台交互已拆分到 cli.c / cli.h，进程入口在 main.c：
 *       未定义 CONSOLE  → 编译为纯内核库，不包含任何 IO/main；
 *       定义了 CONSOLE  → 由 main.c 提供入口，本文件不再包含 main。
 * =========================================================================== */

/* TRUE / FALSE 与内核 bool 类型定义见 Adventure.h（dpCal.c 共用）。 */

/* ------------------------------ 内部数据类型 ------------------------------ */
/* Option / Question / StateVec 的完整定义已随内核接口一起移到 Adventure.h
 * （dpCal.c 需要按 options[] 取规则数据），此处直接复用同名 struct。 */

#define OPT_PER_QUESTION 4U

/* ------------------------------ 题库（只读） ------------------------------ */
/* 每道题 4 个选项，options[0]~options[3] 对应 opt_1~opt_4。 */

// questionType1：4 道题（每道 4 个选项）
static const Question questionType1[] = {
    /* 101 商店购物 */
    {101, 1, {
        {{.luck = 4}   , {.luck = -1, .agility = 1}, {.charm = -1} },  /* opt_1 */
        {{.agility = 4}, {.luck = 1, .agility = -1}, {.charm = -1} },  /* opt_2 */
        {{.luck = 5}   , {.power = 1}              , {.health = -1}},  /* opt_3 */
        {{.charm = 3}  , {.power = 1, .charm = -1} , {.charm = -1} },  /* opt_4 */
    }},
    /* 102 神秘礼物 */
    {102, 1, {
        {{.power = 4}  , {.power = 1}             , {.health = -1, .power = -1}},  /* opt_1 */
        {{.wisdom = 4} , {.health = 1, .power = 1}, {.health = -1, .power = 2} },  /* opt_2 */
        {{.charm = 4}  , {.luck = 1}              , {.charm = -1}              },  /* opt_3 */
        {{.agility = 4}, {.agility = 1}           , {.charm = -1}              },  /* opt_4 */
    }},
    /* 103 没好武器 */
    {103, 1, {
        {{.power = 4} , {.charm = -1}              , {.health = -1}             },  /* opt_1 */
        {{.charm = 3} , {.power = 1, .agility = -1}, {0}                        },  /* opt_2 */
        {{.luck = 5}  , {.power = 1}               , {.charm = -1}              },  /* opt_3 */
        {{.wisdom = 5}, {.power = 2}               , {.health = -1, .power = -1}},  /* opt_4 */
    }},
    /* 104 族长任务 */
    {104, 1, {
        {{.power = 4} , {.power = 1} , {.health = -1}},  /* opt_1 */
        {{.charm = 4} , {0}          , {.health = -1}},  /* opt_2 */
        {{.luck = 4}  , {.charm = -1}, {.wisdom = -1}},  /* opt_3 */
        {{.wisdom = 4}, {.wisdom = 1}, {.charm = -1} },  /* opt_4 */
    }},
};

// questionType2：10 道题（每道 4 个选项）
static const Question questionType2[] = {
    /* 201 玄武门 */
    {201, 2, {
        {{.wisdom = 5} , {.power = 1} , {0}            },  /* opt_1 */
        {{.luck = 5}   , {.charm = -1}, {.charm = -1}  },  /* opt_2 */
        {{.charm = 4}  , {.charm = 1} , {.charm = -1}  },  /* opt_3 */
        {{.agility = 4}, {0}          , {.agility = -1}},  /* opt_4 */
    }},
    /* 202 雪莲幼苗 */
    {202, 2, {
        {{.health = 3} , {.health = 1}             , {.health = -1}               },  /* opt_1 */
        {{.agility = 5}, {.health = 1, .wisdom = 1}, {.health = -1}               },  /* opt_2 */
        {{.wisdom = 5} , {.health = 1, .wisdom = 1}, {0}                          },  /* opt_3 */
        {{.luck = 5}   , {.health = 2}             , {.health = -1, .agility = -1}},  /* opt_4 */
    }},
    /* 203 造梦威廉 */
    {203, 2, {
        {{.power = 4} , {.health = 1}, {.health = -1}},  /* opt_1 */
        {{.wisdom = 4}, {.power = 1} , {.wisdom = 1} },  /* opt_2 */
        {{.charm = 4} , {.charm = 1} , {0}           },  /* opt_3 */
        {{.luck = 4}  , {.wisdom = 1}, {0}           },  /* opt_4 */
    }},
    /* 204 蓝色水鼠 */
    {204, 2, {
        {{.health = 127}, {0}                       , {.health = -1}               },  /* opt_1 */
        {{.charm = 5}   , {.power = 1, .charm = 1}  , {.health = -1}               },  /* opt_2 */
        {{.luck = 5}    , {.power = 1, .luck = 1}   , {.health = -1}               },  /* opt_3 */
        {{.agility = 5} , {.power = 1, .agility = 1}, {.health = -1, .agility = -1}},  /* opt_4 */
    }},
    /* 205 飞天大盗 */
    {205, 2, {
        {{.agility = 5}            , {.power = 2, .charm = -1} , {.health = -1}},  /* opt_1 */
        {{.power = 4, .agility = 3}, {.health = -1, .power = 2}, {0}           },  /* opt_2 */
        {{.wisdom = 5}             , {.wisdom = 1, .luck = 1}  , {.charm = -1} },  /* opt_3 */
        {{.charm = 5}              , {.charm = 1}              , {.charm = -1} },  /* opt_4 */
    }},
    /* 206 老人摔倒 */
    {206, 2, {
        {{.power = 3, .wisdom = 4}, {.wisdom = 1}            , {.health = -1, .charm = -1}},  /* opt_1 */
        {{.charm = 5}             , {.health = 1, .charm = 1}, {.health = -1}             },  /* opt_2 */
        {{.agility = 4}           , {0}                      , {.charm = -1}              },  /* opt_3 */
        {{.health = 127}          , {0}                      , {.wisdom = -1, .charm = -1}},  /* opt_4 */
    }},
    /* 207 圣诞老人 */
    {207, 2, {
        {{.agility = 4} , {.agility = 1}          , {.charm = -1} },  /* opt_1 */
        {{.power = 5}   , {.power = 1}            , {.health = -1}},  /* opt_2 */
        {{.charm = 3}   , {.wisdom = 1, .luck = 1}, {0}           },  /* opt_3 */
        {{.health = 127}, {0}                     , {.wisdom = -1}},  /* opt_4 */
    }},
    /* 208 远洋大船 */
    {208, 2, {
        {{.wisdom = 4}          , {.wisdom = 1, .charm = 1}, {.charm = -1} },  /* opt_1 */
        {{.luck = 5}            , {.health = -1}           , {.health = -1}},  /* opt_2 */
        {{.charm = 4, .luck = 4}, {.luck = 1}              , {.charm = -1} },  /* opt_3 */
        {{.charm = 4}           , {.charm = -1}            , {.health = -1}},  /* opt_4 */
    }},
    /* 209 长安城 */
    {209, 2, {
        {{.wisdom = 4}             , {.wisdom = 1}             , {.charm = -1}  },  /* opt_1 */
        {{.charm = 4, .luck = 3}   , {.power = 1}              , {.charm = -1}  },  /* opt_2 */
        {{.power = 4}              , {.charm = 1}              , {.health = -1} },  /* opt_3 */
        {{.charm = 3, .agility = 4}, {.power = 1, .agility = 1}, {.agility = -1}},  /* opt_4 */
    }},
    /* 210 联盟招人 */
    {210, 2, {
        {{.agility = 5}, {.charm = 1, .agility = 1}, {0}          },  /* opt_1 */
        {{.power = 5}  , {.power = 1, .charm = 1}  , {0}          },  /* opt_2 */
        {{.luck = 4}   , {.power = 1}              , {0}          },  /* opt_3 */
        {{.charm = 4, .wisdom = 3}, {.charm = 2}              , {.charm = -1}},  /* opt_4 */
    }},
};

// questionType3：15 道题（每道 4 个选项）
static const Question questionType3[] = {
    /* 301 平安村 */
    {301, 3, {
        {{.power = 6}  , {.health = 1, .charm = 1}, {.health = -2}               },  /* opt_1 */
        {{.agility = 6}, {.agility = 1}           , {.health = -1, .agility = -1}},  /* opt_2 */
        {{.charm = 6}  , {.power = 1}             , {.health = -1, .charm = -1}  },  /* opt_3 */
        {{.luck = 5}   , {.charm = -1, .luck = 1} , {.health = -1}               },  /* opt_4 */
    }},
    /* 302 彪悍虎精 */
    {302, 3, {
        {{.power = 6}               , {.power = 1}  , {.health = -2}             },  /* opt_1 */
        {{.wisdom = 4, .agility = 5}, {.agility = 1}, {.health = -2}             },  /* opt_2 */
        {{.power = 4, .wisdom = 5}  , {.wisdom = 1} , {.health = -2}             },  /* opt_3 */
        {{.charm = 5, .luck = 4}    , {.charm = 1}  , {.health = -1, .charm = -1}},  /* opt_4 */
    }},
    /* 303 登山黑影 */
    {303, 3, {
        {{.charm = 5, .luck = 4}, {0}                       , {.health = -1, .charm = -1}  },  /* opt_1 */
        {{.power = 6}           , {.health = 1, .power = -1}, {.health = -2}               },  /* opt_2 */
        {{.charm = 6}           , {.health = 1}             , {.health = -1, .agility = -1}},  /* opt_3 */
        {{.agility = 6}         , {.agility = -1}           , {.health = -2}               },  /* opt_4 */
    }},
    /* 304 四头虫 */
    {304, 3, {
        {{.power = 6, .agility = 5}, {.power = 1, .charm = -1}, {.health = -1}},  /* opt_1 */
        {{.power = 7}              , {.health = 1}            , {.health = -1}},  /* opt_2 */
        {{.charm = 7}              , {.charm = 1}             , {.health = -2}},  /* opt_3 */
        {{.agility = 6}            , {.agility = -1}          , {.health = -2}},  /* opt_4 */
    }},
    /* 305 童子炼丹 */
    {305, 3, {
        {{.power = 5, .luck = 5}  , {.health = 1, .power = 1} , {.health = -1}               },  /* opt_1 */
        {{.charm = 6}             , {.health = 1, .power = 1} , {0}                          },  /* opt_2 */
        {{.luck = 5, .agility = 5}, {.power = 1, .agility = 1}, {.health = -1, .agility = -1}},  /* opt_3 */
        {{.health = 127}          , {0}                       , {.charm = -2}                },  /* opt_4 */
    }},
    /* 306 幼童气球 */
    {306, 3, {
        {{.health = 127}, {0}                    , {.health = -1, .charm = -1}},  /* opt_1 */
        {{.agility = 6} , {.charm = 1, .luck = 1}, {.agility = -1}            },  /* opt_2 */
        {{.charm = 5}   , {.charm = 1}           , {.charm = -1}              },  /* opt_3 */
        {{.luck = 5}    , {.charm = -1}          , {.health = -1}             },  /* opt_4 */
    }},
    /* 307 朱雀街 */
    {307, 3, {
        {{.power = 6}               , {.power = 1}                , {.health = -2}               },  /* opt_1 */
        {{.wisdom = 4, .charm = 5}  , {.charm = 1}                , {.health = -2}               },  /* opt_2 */
        {{.wisdom = 5, .agility = 4}, {.health = -1, .agility = 1}, {.health = -2}               },  /* opt_3 */
        {{.luck = 4, .agility = 4}  , {.health = -1}              , {.health = -1, .agility = -1}},  /* opt_4 */
    }},
    /* 308 联盟阵营 */
    {308, 3, {
        {{.luck = 6}   , {.luck = 1}   , {0}},  /* opt_1 */
        {{.power = 6}  , {.power = 1}  , {0}},  /* opt_2 */
        {{.agility = 6}, {.agility = 1}, {0}},  /* opt_3 */
        {{.health = 6} , {.health = 1} , {0}},  /* opt_4 */
    }},
    /* 309 锦绣峰 */
    {309, 3, {
        {{.luck = 6}   , {.health = 1}, {.charm = -1}                },  /* opt_1 */
        {{.agility = 6}, {.charm = -1}, {.health = -1, .agility = -1}},  /* opt_2 */
        {{.power = 6}  , {.charm = 1} , {.health = -1, .power = -1}  },  /* opt_3 */
        {{.charm = 6}  , {.wisdom = 1}, {.health = -1}               },  /* opt_4 */
    }},
    /* 310 深夜破庙 */
    {310, 3, {
        {{.luck = 5}   , {.health = -1, .power = -1}, {.health = -1, .agility = -1}             },  /* opt_1 */
        {{.wisdom = 6} , {.wisdom = 1}              , {0}                                       },  /* opt_2 */
        {{.health = 5} , {.health = -1}             , {.health = -1, .power = -1, .agility = -1}},  /* opt_3 */
        {{.agility = 5}, {.wisdom = 1}              , {.agility = -1}                           },  /* opt_4 */
    }},
    /* 311 时光之舟 */
    {311, 3, {
        {{.charm = 7} , {.power = 2}             , {.power = 1} },  /* opt_1 */
        {{.power = 7} , {.power = 1, .wisdom = 1}, {.wisdom = 1}},  /* opt_2 */
        {{.luck = 6}  , {.luck = 1}              , {0}          },  /* opt_3 */
        {{.wisdom = 6}, {.wisdom = 1}            , {0}          },  /* opt_4 */
    }},
    /* 312 十八罗汉 */
    {312, 3, {
        {{.power = 5, .wisdom = 4}, {.charm = 1}              , {.health = -1}},  /* opt_1 */
        {{.wisdom = 6}            , {.health = 1, .wisdom = 1}, {.health = 1} },  /* opt_2 */
        {{.luck = 6}              , {.luck = 1}               , {.health = -1}},  /* opt_3 */
        {{.charm = 6}             , {.power = 1}              , {.charm = -1} },  /* opt_4 */
    }},
    /* 313 七仙女 */
    {313, 3, {
        {{.charm = 6}   , {.charm = 2}, {0}           },  /* opt_1 */
        {{.health = 127}, {0}         , {.health = -1}},  /* opt_2 */
        {{.luck = 6}    , {0}         , {0}           },  /* opt_3 */
        {{.health = 127}, {0}         , {.health = -1}},  /* opt_4 */
    }},
    /* 314 梼杌外挂 */
    {314, 3, {
        {{0}            , {.power = 1, .wisdom = 1}, {0}                        },  /* opt_1 */
        {{0}            , {.wisdom = 1, .charm = 1}, {0}                        },  /* opt_2 */
        {{.health = 127}, {0}                      , {.health = -9, .charm = -9}},  /* opt_3 */
        {{.health = 127}, {0}                      , {.health = -1, .charm = -1}},  /* opt_4 */
    }},
    /* 315 老者打坐 */
    {315, 3, {
        {{0}         , {.wisdom = 1} , {0}           },  /* opt_1 */
        {{.power = 7}, {.power = 1}  , {.health = -1}},  /* opt_2 */
        {{0}         , {.agility = 1}, {0}           },  /* opt_3 */
        {{0}         , {.luck = 1}   , {0}           },  /* opt_4 */
    }},
};

// questionType4：7 道题（每道 4 个选项）
static const Question questionType4[] = {
    /* 401 琉璃宫 */
    {401, 4, {
        {{.wisdom = 6, .agility = 8}, {.wisdom = 1, .agility = 1}, {.health = -1}},  /* opt_1 */
        {{.luck = 9}                , {.luck = 1}                , {.health = -2}},  /* opt_2 */
        {{.wisdom = 8, .agility = 6}, {.wisdom = 1, .agility = -1}, {.health = -1}},  /* opt_3 */
        {{.power = 8}               , {.health = -1, .power = 1} , {.health = -1}},  /* opt_4 */
    }},
    /* 402 唐僧受伤 */
    {402, 4, {
        {{.charm = 8}   , {.luck = 1}               , {.health = -1}},  /* opt_1 */
        {{.agility = 9} , {.power = 1, .agility = 1}, {.health = -2}},  /* opt_2 */
        {{.wisdom = 8}  , {.power = 1, .charm = -1} , {.health = -1}},  /* opt_3 */
        {{.health = 127}, {0}                       , {.health = -1}},  /* opt_4 */
    }},
    /* 403 哮天犬 */
    {403, 4, {
        {{.health = 127}            , {0}           , {.health = -1, .agility = -1}},  /* opt_1 */
        {{.agility = 8}             , {.agility = 1}, {.wisdom = -1}               },  /* opt_2 */
        {{.charm = 8}               , {.power = 1, .charm = 1}, {.charm = -1}},  /* opt_3 */
        {{.luck = 8}                , {.health = 1} , {.charm = -1}                },  /* opt_4 */
    }},
    /* 404 幻镜阁 */
    {404, 4, {
        {{.power = 9} , {.power = 1} , {.health = -1}             },  /* opt_1 */
        {{.wisdom = 8}, {.wisdom = 1}, {.health = -1, .charm = -1}},  /* opt_2 */
        {{.luck = 8}  , {.luck = 1}  , {.health = -2}             },  /* opt_3 */
        {{.charm = 9} , {.charm = 1} , {.agility = -1}            },  /* opt_4 */
    }},
    /* 405 老者下棋 */
    {405, 4, {
        {{.wisdom = 9} , {.power = 1, .wisdom = 1}, {.charm = -1} },  /* opt_1 */
        {{.power = 8}  , {.charm = 1}             , {.health = -1}},  /* opt_2 */
        {{.agility = 8}, {.charm = 1}             , {0}           },  /* opt_3 */
        {{.luck = 9}   , {.power = 1, .wisdom = 1}, {.health = -1}},  /* opt_4 */
    }},
    /* 406 观音通天 */
    {406, 4, {
        {{.power = 9}             , {.charm = 2}               , {.health = -2}             },  /* opt_1 */
        {{.power = 6, .charm = 8} , {.charm = -1, .agility = 2}, {.health = -1, .charm = -1}},  /* opt_2 */
        {{.power = 6, .wisdom = 8}, {.power = 2, .charm = -1}  , {.health = -2, .charm = -2}},  /* opt_3 */
        {{.luck = 6, .agility = 8}, {.health = -1, .agility = 1} , {.health = -2}             },  /* opt_4 */
    }},
    /* 407 螺旋地狱 */
    {407, 4, {
        {{.power = 9}            , {.power = 1, .wisdom = 1}                          , {.health = -2, .power = 1}   },  /* opt_1 */
        {{.agility = 8}          , {.agility = 1}                                     , {.health = -1, .agility = -1}},  /* opt_2 */
        {{.wisdom = 6, .luck = 7}, {.health = 1, .wisdom = 1}                         , {0}                          },  /* opt_3 */
        {{.charm = 9}            , {.power = 1, .wisdom = 1, .charm = 1, .agility = 1}, {.health = -2}               },  /* opt_4 */
    }},
};

// questionType5：5 道题（每道 4 个选项）
static const Question questionType5[] = {
    /* 501 金之祖巫 */
    {501, 5, {
        {{.power = 10}              , {0}, {.health = -3}},  /* opt_1 */
        {{.charm = 10}              , {0}, {.health = -3}},  /* opt_2 */
        {{.wisdom = 7, .agility = 9}, {0}, {.health = -3}},  /* opt_3 */
        {{.luck = 7, .agility = 9}  , {0}, {.health = -3}},  /* opt_4 */
    }},
    /* 502 木之祖巫 */
    {502, 5, {
        {{.wisdom = 9, .luck = 7}  , {0}, {.health = -3}},  /* opt_1 */
        {{.power = 9, .agility = 7}, {0}, {.health = -3}},  /* opt_2 */
        {{.health = 127}           , {0}, {.health = -3}},  /* opt_3 */
        {{.health = 127}           , {0}, {.health = -3}},  /* opt_4 */
    }},
    /* 503 水之祖巫 */
    {503, 5, {
        {{.luck = 8, .agility = 8}, {0}, {.health = -3}},  /* opt_1 */
        {{.power = 10}            , {0}, {.health = -3}},  /* opt_2 */
        {{.power = 7, .wisdom = 9}, {0}, {.health = -3}},  /* opt_3 */
        {{.charm = 10}            , {0}, {.health = -3}},  /* opt_4 */
    }},
    /* 504 火之祖巫 */
    {504, 5, {
        {{.charm = 9, .agility = 7}, {0}, {.health = -3}},  /* opt_1 */
        {{.agility = 10}           , {0}, {.health = -3}},  /* opt_2 */
        {{.wisdom = 10}            , {0}, {.health = -3}},  /* opt_3 */
        {{.power = 10}             , {0}, {.health = -3}},  /* opt_4 */
    }},
    /* 505 通天教主 */
    {505, 5, {
        {{.power = 10}              , {0}, {.health = -3}},  /* opt_1 */
        {{.power = 7, .wisdom = 9}  , {0}, {.health = -3}},  /* opt_2 */
        {{.wisdom = 9, .luck = 7}   , {0}, {.health = -3}},  /* opt_3 */
        {{.wisdom = 7, .agility = 9}, {0}, {.health = -3}},  /* opt_4 */
    }},
};

/* ------------------------------ 角色初始属性（只读） ------------------------------ */
static const Attrs g_initAttrs[7] = {
    /* 索引 = RoleId - 1 */
    {.health = 3, .power = 4, .wisdom = 5, .charm = 5, .luck = 4, .agility = 3},  /* 小唐 */
    {.health = 4, .power = 5, .wisdom = 4, .charm = 3, .luck = 4, .agility = 4},  /* 小猴 */
    {.health = 6, .power = 4, .wisdom = 3, .charm = 3, .luck = 3, .agility = 3},  /* 小猪 */
    {.health = 3, .power = 3, .wisdom = 4, .charm = 3, .luck = 3, .agility = 6},  /* 小沙 */
    {.health = 4, .power = 3, .wisdom = 4, .charm = 4, .luck = 5, .agility = 4},  /* 小白 */
    {.health = 3, .power = 3, .wisdom = 4, .charm = 5, .luck = 3, .agility = 6},  /* 小玉 */
    {.health = 5, .power = 4, .wisdom = 4, .charm = 4, .luck = 3, .agility = 4}   /* 小吒 */
};
const Attrs *const g_pInitAttrs = g_initAttrs;

/* ------------------------------ 规则常量（只读） ------------------------------ */
/* gid 0~4：题型阶段，见 judgeGroup */
const int totalQs[5] = { 4, 10, 15, 7, 5 };
const int needPick[5] = { 1, 2, 3, 3, 1 };

/* 各题型成功 / 失败得分 */
const int successScore[5] = { 6, 6, 9, 12, 19 };
const int failScore[5]    = { 3, 3, 4, 6, 9 };

/* maxRemainingScore[done_cnt]：剩余题目全成功时的最高分，用作 dpGoalProb 剪枝 */
const int maxRemainingScore[11] = {
    100, 94, 88, 82, 73, 64, 55, 43, 31, 19, 0
};

/* ------------------------------ 纯工具函数（无副作用） ------------------------------ */
typedef int bool;

/*
 * 判断当前属性是否满足某选项的成功判定门槛。
 * 参数：cur - 当前属性；cond - 所需门槛属性
 * 返回：所有维度均 >= 门槛返回 TRUE，否则 FALSE
 */
static inline bool isSuccess(Attrs cur, Attrs cond)
{
    return (cur.health >= cond.health) && (cur.power >= cond.power) && (cur.wisdom >= cond.wisdom)
        && (cur.charm >= cond.charm) && (cur.luck >= cond.luck) && (cur.agility >= cond.agility);
}

/* 应用属性增减：pwcla 下限钳位 0；health 不钳位允许负数 */
Attrs apply_attr(const Attrs old, const Attrs delta)
{
    Attrs out;
    out.health = (int8_t)((int)old.health + delta.health);
    int v;
    v = (int)old.power + delta.power;   out.power   = (int8_t)(v < 0 ? 0 : v);
    v = (int)old.wisdom + delta.wisdom; out.wisdom  = (int8_t)(v < 0 ? 0 : v);
    v = (int)old.charm + delta.charm;   out.charm   = (int8_t)(v < 0 ? 0 : v);
    v = (int)old.luck + delta.luck;     out.luck    = (int8_t)(v < 0 ? 0 : v);
    v = (int)old.agility + delta.agility; out.agility = (int8_t)(v < 0 ? 0 : v);
    return out;
}

/* done_cnt -> 题型阶段 0~4 */
int judgeGroup(int done_cnt)
{
    switch (done_cnt) {
        case 0:            return 0;
        case 1: case 2:    return 1;
        case 3: case 4: case 5: return 2;
        case 6: case 7: case 8: return 3;
        case 9:            return 4;
        default:           return -1; /* done_cnt==10 */
    }
}

/*
 * 将题目下标 q_idx 转换为对应位的 bitmask。
 * 参数：q_idx - 本组内题目下标（0 起）
 * 返回：仅第 q_idx 位为 1 的 uint16_t 掩码
 */
uint16_t convert_group_mask(int q_idx)
{
    return (uint16_t)(1U << q_idx);
}

/*
 * 统计 bitmask 中置位（为 1）的位数。
 * 参数：mask - 位掩码
 * 返回：置位数量
 */
int mask_count(uint16_t mask)
{
    int c = 0;
    while (mask != 0U) { c += (int)(mask & 1U); mask >>= 1; }
    return c;
}

/*
 * 根据阶段编号返回对应题库数组的首地址。
 * 参数：type - 阶段编号 0~4
 * 返回：该阶段 Question 数组指针；非法 type 返回 NULL
 */
const Question* question_db_for_type(int type)
{
    switch (type) {
        case 0: return questionType1;
        case 1: return questionType2;
        case 2: return questionType3;
        case 3: return questionType4;
        case 4: return questionType5;
        default: return NULL;
    }
}

/* ===================== key 编码 ===================== */
/* 属性上限 PWCLA_MAX / HEALTH_MAX、哨兵 NO_REMAIN_AIM_SCORE 见 Adventure.h；
 * 哈希表的占用标记 KEY_IS_USED 见 hash.h。 */

/* 把 group_mask + 属性 + done_cnt + r 打包成 uint64_t flat_key；
 * health<=0 返回 UINT64_MAX 代表无效死亡状态。
 * bit分配：[0-15]mask [16-19]health-1 [20-23]power [24-27]wisdom
 *          [28-31]charm [32-35]luck [36-39]agility [40-43]done [44-51]able_lost_s 
 *          [52-62]未使用
 *          [63]必须为0，哈希使用标记，哈希函数没有检查，请保证必须为0
 * 注意：v2以前版本的[44-51]定义为r，即还需要得的分数
 *       这里的able_lost_s为还能丢的分数（key的结构无需整体改动，但这里含义改变）
 */
uint64_t encode_flat_key(uint16_t group_mask, const Attrs raw_attr,
                         uint8_t done_cnt, uint8_t remain_aim_score)
{
    if (raw_attr.health <= 0)
        return UINT64_MAX;

    uint64_t h = (uint64_t)raw_attr.health - 1U;
    if (h > 9U) h = 9U;

    uint64_t p = (uint64_t)raw_attr.power;
    uint64_t w = (uint64_t)raw_attr.wisdom;
    uint64_t c = (uint64_t)raw_attr.charm;
    uint64_t l = (uint64_t)raw_attr.luck;
    uint64_t a = (uint64_t)raw_attr.agility;
    if (p > PWCLA_MAX) p = PWCLA_MAX;
    if (w > PWCLA_MAX) w = PWCLA_MAX;
    if (c > PWCLA_MAX) c = PWCLA_MAX;
    if (l > PWCLA_MAX) l = PWCLA_MAX;
    if (a > PWCLA_MAX) a = PWCLA_MAX;

    uint64_t key = 0;
    key |= (uint64_t)group_mask;
    key |= h << 16U;
    key |= p << 20U;
    key |= w << 24U;
    key |= c << 28U;
    key |= l << 32U;
    key |= a << 36U;
    key |= (uint64_t)done_cnt << 40U;
    key |= (uint64_t)remain_aim_score << 44U;
    return key;
}

/* ===================== 剪枝与单步评估（无副作用） ===================== */
/* StateVec 的定义见 Adventure.h（dpCal.c 共用）。 */

/* b 是否帕累托支配 a（b 全维度 >= a） */
static inline bool pareto_dominates(const StateVec *b, const StateVec *a)
{
    return (b->h >= a->h) && (b->p >= a->p) && (b->w >= a->w)
        && (b->c >= a->c) && (b->l >= a->l) && (b->a >= a->a) && (b->sc >= a->sc);
}

/*
 * 评估单个选项：判定成功/失败，计算结算后的属性与得分标记。
 * 参数：cur_attr - 当前属性；q - 题目指针；opt_idx - 选项下标 0~3
 * 返回：包含结算后六维属性与是否成功标记的 StateVec
 */
static inline StateVec eval_option(const Attrs cur_attr, const Question *q, int opt_idx)
{
    StateVec res;
    const Option *opt = &q->options[opt_idx];
    bool ok = isSuccess(cur_attr, opt->successAttrs);
    Attrs final_attr = apply_attr(cur_attr, ok ? opt->successAward : opt->failPunish);
    res.h = final_attr.health;
    res.p = final_attr.power;
    res.w = final_attr.wisdom;
    res.c = final_attr.charm;
    res.l = final_attr.luck;
    res.a = final_attr.agility;
    res.sc = ok ? 1 : 0;
    return res;
}

/* 选项剪枝：删掉被支配或必死且非最后题的选项。改动 vecs/keep，无外部副作用 */
void prune_options(const Question *q, const Attrs raw_attr,
                   int done_cnt, StateVec vecs[4], bool keep[4])
{
    for (int opt = 0; opt < 4; opt++) {
        vecs[opt] = eval_option(raw_attr, q, opt);
        keep[opt] = TRUE;
    }
    bool is_dead[4];
    for (int opt = 0; opt < 4; opt++)
        is_dead[opt] = (vecs[opt].h <= 0);
    bool is_last_question = (done_cnt == 9);

    for (int ai_opt = 0; ai_opt < 4; ai_opt++) {
        if (!keep[ai_opt]) continue;
        for (int cand = 0; cand < 4; cand++) {
            if (ai_opt == cand || !keep[cand]) continue;
            if (is_dead[ai_opt] && !is_dead[cand] && !is_last_question) {
                keep[ai_opt] = FALSE;
                break;
            }
            if (pareto_dominates(&vecs[cand], &vecs[ai_opt])) {
                keep[ai_opt] = FALSE;
                break;
            }
        }
    }
}

/* =====================================================================
 * DP 计算本体（dpMaxAverage / dpGoalProb）已拆分到 dpCal.c / dpCal.h。
 * 本文件只保留它们的调用方与规则常量，不再包含 DP 实现。
 * ===================================================================== */

 /* =======================   磁盘打印   =============================== */
#define CACHE_MAGIC     0x4D454D4FU  // 校验码

/*
 * 从磁盘文件加载 DP 缓存到哈希表。
 * 校验 magic/version/role_id，任一不匹配或数据不完整则判定损坏并返回 -1。
 * 参数：path - 缓存文件路径；role_id - 角色 ID；h - 目标哈希表
 * 返回：0 成功；-1 失败（文件不存在/版本不符/数据损坏）
 */
static int cache_load_file(const char *path, int role_id, MemoHash *h)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    uint32_t magic, version, rid, cnt;
    if (fread(&magic, 4, 1, fp) != 1 || magic != CACHE_MAGIC) { fclose(fp); return -1; }
    if (fread(&version, 4, 1, fp) != 1 || version != GAME_VERSION) { fclose(fp); return -1; }
    if (fread(&rid, 4, 1, fp) != 1 || rid != (uint32_t)role_id) { fclose(fp); return -1; }
    if (fread(&cnt, 4, 1, fp) != 1) { fclose(fp); return -1; }

    memo_hash_init_s(h, (size_t)cnt * 2, 1 << 20);
    if (!h->table) { fclose(fp); return -1; }

    for (uint32_t i = 0; i < cnt; i++) {
        uint64_t key;
        double val;
        if (fread(&key, 8, 1, fp) != 1) break;
        if (fread(&val, 8, 1, fp) != 1) break;
        memo_hash_put(h, key & ~KEY_IS_USED, val, TRUE);
    }
    fclose(fp);

    if (h->used_cnt != cnt) {   /* 加载不完整，判定损坏 */
        memo_hash_destroy(h);
        return -1;
    }
    return 0;
}

/*
 * 将哈希表中的 DP 缓存写入磁盘文件。
 * 参数：path - 缓存文件路径；role_id - 角色 ID；h - 源哈希表
 * 返回：0 成功；-1 文件打开失败
 */
static int cache_save_file(const char *path, int role_id, MemoHash *h)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    uint32_t magic = CACHE_MAGIC;
    uint32_t version = GAME_VERSION;
    uint32_t rid = (uint32_t)role_id;
    uint32_t cnt = (uint32_t)h->used_cnt;

    fwrite(&magic, 4, 1, fp);
    fwrite(&version, 4, 1, fp);
    fwrite(&rid, 4, 1, fp);
    fwrite(&cnt, 4, 1, fp);

    for (size_t i = 0; i < h->capacity; i++) {
        if (h->table[i].key) {
            uint64_t tmp = h->table[i].key & ~KEY_IS_USED;
            fwrite(&tmp, 8, 1, fp);
            fwrite(&h->table[i].val, 8, 1, fp);
        }
    }
    fclose(fp);
    return 0;
}

/* 期望分缓存：init_<role>_expect.cache */
static int cache_load_expect(int role_id, MemoHash *h)
{
    char path[64];
    snprintf(path, sizeof(path), "init_%d_expect.cache", role_id);
    return cache_load_file(path, role_id, h);
}

/*
 * 保存期望分缓存到 init_<role>_expect.cache。
 * 参数：role_id - 角色 ID；h - 源哈希表
 * 返回：0 成功；-1 失败
 */
static int cache_save_expect(int role_id, MemoHash *h)
{
    char path[64];
    snprintf(path, sizeof(path), "init_%d_expect.cache", role_id);
    return cache_save_file(path, role_id, h);
}

/* struct GameSnapshot 已移到 Adventure.h（JS 需在 wasm 堆上填充后传入，
 * 不能是不透明类型）。 */

/* =====================================================================
 * GameContext：一局游戏的全部可变状态
 *
 * 【位置约束·不要上移】成员依赖 MemoHash 与 DebugStats（均已在
 * Adventure.h / hash.h 中给出完整定义），且头文件里 GameContext 只是
 * 不透明 typedef，因此本定义必须出现在 game_ctx_create_form /
 * game_ctx_create 等所有使用点之前。
 * ===================================================================== */
struct GameContext {
    RoleId   role_id;

    MemoHash memo_expect;   /* DP 缓存：期望最大分数 memo 哈希表 */
    int      expect_ready;  /* 1=期望分哈希表已初始化就绪 */
    int      dirty_expect;  /* 1=有新增结果，销毁时需落盘 */

    MemoHash memo_prob;     /* DP 缓存：达成目标 T 的概率 memo 哈希表 */
    int      prob_ready;    /* 1=概率哈希表已初始化就绪 */
    // int      dirty_prob;    /* 1=有新增结果，销毁时需落盘 */

    MemoHash memo_proMax620;/* DP 缓存：冲刺模式达成目标 620 的概率 memo 哈希表 */
    int      proMax620_ready;/* 1=概率哈希表已初始化就绪 */

    MemoHash memo_proMax480;/* DP 缓存：冲刺模式达成目标 480 的概率 memo 哈希表 */
    int      proMax480_ready;/* 1=概率哈希表已初始化就绪 */

    Attrs    cur_attr;
    uint16_t group_mask;    /* 本组已选题目掩码（bit 标记） */
    int      done_cnt;      /* 已完成题数 0~10 */
    int      total_score;   /* 当前累计总得分 */
    
    #ifdef DEBUG
    DebugStats stats;       /* memo 命中 / 未命中计数 */
    #endif

};

static inline int clamp_pwcla(int val)
{
    if (val < 0) return 0;
    if (val > 12) return 12;
    return val;
}

/* ============================ 对外 API ============================ */

GameContext *game_ctx_create_form(GameSnapshot *s)
{
    if (!s) return NULL;
    if (s->done_cnt >= 10) return NULL;
    if (s->score > 100) return NULL;
    if (s->attrs.health <= 0) return NULL;

    GameContext *ctx = (GameContext *)calloc(1, sizeof(GameContext));
    if (!ctx) return NULL;
    ctx->role_id = ROLE_USER_DEFINE;
 

    ctx->cur_attr.health = (s->attrs.health > 8) ? 8 : s->attrs.health;
    ctx->cur_attr.power = clamp_pwcla(s->attrs.power);
    ctx->cur_attr.wisdom = clamp_pwcla(s->attrs.wisdom);
    ctx->cur_attr.charm = clamp_pwcla(s->attrs.charm);
    ctx->cur_attr.luck = clamp_pwcla(s->attrs.luck);
    ctx->cur_attr.agility = clamp_pwcla(s->attrs.agility);

    ctx->group_mask = 0;
    for (int i = 0; i < totalQs[judgeGroup(s->done_cnt)]; ++i)
        ctx->group_mask |= (s->qid_selected[i] << i);

    ctx->done_cnt = s->done_cnt;
    ctx->total_score = s->score;

    /* 尝试加载期望缓存；失败则开空表 */
    if (cache_load_expect(ctx->role_id, &ctx->memo_expect) == 0) {
        ctx->expect_ready = 1;
    }
    else {
        if (memo_hash_init(&ctx->memo_expect, 1U << 20))
            ctx->expect_ready = 0;
        else
            ctx->expect_ready = 1;
    }

    /* 尝试加载达标概率缓存；失败则开空表 */
    // 功能更新，暂时删除文件加载
    if (memo_hash_init(&ctx->memo_prob, 1U << 20))
        ctx->prob_ready = 0;
    else
        ctx->prob_ready = 1;

    ctx->proMax480_ready = 0;

    ctx->proMax620_ready = 0;

    ctx->memo_proMax480.table = NULL;
    ctx->proMax480_ready = 0;

    ctx->memo_proMax480.table = NULL;
    ctx->proMax620_ready = 0;

    return ctx;
}

/*
 * 对外 API：在快照 s 所关联的**已有** GameContext 上原地改写当前局面。
 * 处理逻辑与 game_ctx_create_form 完全一致（同样的合法性校验、同样的属性裁剪、
 * 同样的 group_mask / done_cnt / total_score 写入），区别在于：
 *   - 不新建 GameContext，因此不重新 calloc 两张 memo 哈希表，也不重新走
 *     磁盘缓存加载，已有的 DP 计算结果全部保留复用；
 *   - **不改动 ctx->role_id**，即不会覆写成 ROLE_USER_DEFINE，
 *     原有角色身份（含其缓存归属）保持不变。
 * 参数：s - 局面快照；其中 s->ctx 为目标上下文，必须非空
 * 返回：0 成功；-1 参数非法（s 为空 / s->ctx 为空 / done_cnt>9 / score>100 /
 *      health<=0）
 */
int game_ctx_modify(GameSnapshot *s)
{
    if (!s) return -1;
    if (!s->ctx) return -1;
    if (s->done_cnt >= 10) return -1;
    if (s->score > 100) return -1;
    if (s->attrs.health <= 0) return -1;

    GameContext *ctx = s->ctx;

    /* 属性写入：与 create_form 同一套裁剪规则 */
    ctx->cur_attr.health  = (s->attrs.health > 8) ? 8 : s->attrs.health;
    ctx->cur_attr.power   = clamp_pwcla(s->attrs.power);
    ctx->cur_attr.wisdom  = clamp_pwcla(s->attrs.wisdom);
    ctx->cur_attr.charm   = clamp_pwcla(s->attrs.charm);
    ctx->cur_attr.luck    = clamp_pwcla(s->attrs.luck);
    ctx->cur_attr.agility = clamp_pwcla(s->attrs.agility);

    ctx->group_mask = 0;
    for (int i = 0; i < totalQs[judgeGroup(s->done_cnt)]; ++i)
        ctx->group_mask |= (s->qid_selected[i] << i);

    ctx->done_cnt    = s->done_cnt;
    ctx->total_score = s->score;

    return 0;
}

/*
 * 对外 API：返回指定角色的初始属性常量指针。
 * 参数：role_id - 角色枚举 ID
 * 返回：初始属性指针；非法 role_id 返回 NULL
 */
const Attrs* game_role_init_attrs(RoleId role_id)
{
    if (role_id < ROLE_XIAO_TANG || role_id > ROLE_XIAO_ZHA)
        return NULL;
    return &g_initAttrs[role_id - 1];
}

/*
 * 对外 API：创建一局游戏的 GameContext，初始化属性、计分与两张 memo 缓存。
 * 优先从磁盘加载已有 DP 缓存；加载失败则开空表等待运行时填充。
 * 参数：role_id - 角色枚举 ID
 * 返回：新建的 GameContext 指针；参数非法或内存不足返回 NULL
 */
GameContext* game_ctx_create(RoleId role_id)
{
    if (role_id < ROLE_XIAO_TANG || role_id > ROLE_XIAO_ZHA)
        return NULL;

    GameContext *ctx = (GameContext *)calloc(1, sizeof(GameContext));
    if (!ctx) return NULL;

    ctx->role_id = role_id;
    ctx->cur_attr = *game_role_init_attrs(role_id);
    ctx->group_mask = 0;
    ctx->done_cnt = 0;
    ctx->total_score = 0;

    
    /* 尝试加载期望缓存；失败则开空表 */
    if (cache_load_expect(role_id, &ctx->memo_expect) == 0) {
        ctx->expect_ready = 1;
    }
    else {
        if (memo_hash_init(&ctx->memo_expect, 1U << 20))
            ctx->expect_ready = 0;
        else
            ctx->expect_ready = 1;
    }

    /* 尝试加载达标概率缓存；失败则开空表 */
    // 功能更新，暂时删除文件加载
    if (memo_hash_init(&ctx->memo_prob, 1U << 20))
        ctx->prob_ready = 0;
    else
        ctx->prob_ready = 1;

    ctx->memo_proMax480.table = NULL;
    ctx->proMax480_ready = 0;

    ctx->memo_proMax480.table = NULL;
    ctx->proMax620_ready = 0;

    return ctx;
}

/*
 * 对外 API：清理掉所有缓存数据，并重新加载模式
 */
void change_game_ctx_mode(GameContext *ctx, bool n_exp_ready, bool n_prob_ready, bool n_480_ready, bool n_620_ready)
{
    if (ctx->expect_ready)    memo_hash_destroy(&ctx->memo_expect);
    if (ctx->prob_ready)      memo_hash_destroy(&ctx->memo_prob);
    if (ctx->proMax480_ready) memo_hash_destroy(&ctx->memo_proMax480);
    if (ctx->proMax620_ready) memo_hash_destroy(&ctx->memo_proMax620);

    if (n_exp_ready)
    {
        /* 尝试加载期望缓存；失败则开空表 */
        if (cache_load_expect(ctx->role_id, &ctx->memo_expect) == 0) {
            ctx->expect_ready = 1;
        }
        else {
            if (memo_hash_init(&ctx->memo_expect, 1U << 20))
                ctx->expect_ready = 0;
            else
                ctx->expect_ready = 1;
        }
    }
    if (n_prob_ready)
    {
        if (memo_hash_init(&ctx->memo_prob, 1U << 20))
            ctx->prob_ready = 0;
        else
            ctx->prob_ready = 1;
    }
    if (n_480_ready)
    {
        if (memo_hash_init(&ctx->memo_proMax480, 1U << 20))
            ctx->proMax480_ready = 0;
        else
            ctx->proMax480_ready = 1;
    }
    else {
        ctx->proMax480_ready = 0;
    }

    if (n_620_ready)
    {
        if (memo_hash_init(&ctx->memo_proMax620, 1U << 20))
            ctx->proMax620_ready = 0;
        else
            ctx->proMax620_ready = 1;
    }
    else {
        ctx->proMax620_ready = 0;
    }
}

GameContext *game_ctx_create_sprint(RoleId role_id)
{
    GameContext *ctx = game_ctx_create(role_id);
    if (!ctx) return NULL;
    change_game_ctx_mode(ctx, TRUE, FALSE, TRUE, TRUE);
    return ctx;
}

GameContext *game_ctx_create_sprint_form(GameSnapshot *s)
{
    GameContext *ctx = game_ctx_create_form(s);
    if (!ctx) return NULL;
    change_game_ctx_mode(ctx, TRUE, FALSE, TRUE, TRUE);
    return ctx;
}

/*
 * 对外 API：销毁 GameContext，若缓存有更新则写回磁盘，然后释放内存。
 * 参数：ctx - 游戏上下文指针（可为 NULL）
 */
void game_ctx_destroy(GameContext *ctx)
{
    if (!ctx) return;

    if (ctx->expect_ready && ctx->dirty_expect)
        cache_save_expect(ctx->role_id, &ctx->memo_expect);
    //if (ctx->prob_ready && ctx->dirty_prob)
    //    cache_save_goal(ctx->role_id, &ctx->memo_prob);

    if (ctx->expect_ready)    memo_hash_destroy(&ctx->memo_expect);
    if (ctx->prob_ready)      memo_hash_destroy(&ctx->memo_prob);
    if (ctx->proMax480_ready) memo_hash_destroy(&ctx->memo_proMax480);
    if (ctx->proMax620_ready) memo_hash_destroy(&ctx->memo_proMax620);
    
    free(ctx);
    ctx = NULL;
}

/*
 * 对外 API：获取当前角色属性。
 * 参数：ctx - 游戏上下文
 * 返回：当前属性指针；ctx 为 NULL 返回 NULL
 */
const Attrs* game_ctx_get_attrs(const GameContext *ctx)
{
    return ctx ? &ctx->cur_attr : NULL;
}
/*
 * 对外 API：获取已完成题目数。
 * 返回：已完成题数；ctx 为 NULL 返回 -1
 */
int game_ctx_get_done_cnt(const GameContext *ctx)
{
    return ctx ? ctx->done_cnt : -1;
}
/*
 * 对外 API：获取当前累计总分。
 * 返回：总得分；ctx 为 NULL 返回 -1
 */
int game_ctx_get_total_score(const GameContext *ctx)
{
    return ctx ? ctx->total_score : -1;
}
/*
 * 对外 API：获取当前所处阶段编号（0~4）。
 * 返回：阶段编号；ctx 为 NULL 或已结束返回 -1
 */
int game_ctx_get_type(const GameContext *ctx)
{
    return ctx ? judgeGroup(ctx->done_cnt) : -1;
}
/*
 * 对外 API：获取当前本组已选题位掩码。
 * 返回：group_mask；ctx 为 NULL 返回 0
 */
uint16_t game_ctx_get_mask(const GameContext *ctx)
{
    return ctx ? ctx->group_mask : 0;
}

/*
 * 对外 API：获取当前阶段（题型）的题目总数。
 * 返回：题目总数；非法状态返回 -1
 */
int game_ctx_type_total(const GameContext *ctx)
{
    if (!ctx) return -1;
    int type = judgeGroup(ctx->done_cnt);
    return (type < 0) ? -1 : totalQs[type];
}
/*
 * 对外 API：获取当前阶段需要选定的题目数（needPick）。
 * 返回：需选题数；非法状态返回 -1
 */
int game_ctx_type_need(const GameContext *ctx)
{
    if (!ctx) return -1;
    int type = judgeGroup(ctx->done_cnt);
    return (type < 0) ? -1 : needPick[type];
}
/*
 * 对外 API：判断本组第 q_idx 题是否尚未被选、当前可选。
 * 参数：q_idx - 本组内题目下标
 * 返回：1 可选；0 已选过或参数非法
 */
int game_ctx_type_available(const GameContext *ctx, int q_idx)
{
    if (!ctx || q_idx < 0) return 0;
    int type = judgeGroup(ctx->done_cnt);
    if (type < 0 || q_idx >= totalQs[type]) return 0;
    return (ctx->group_mask & convert_group_mask(q_idx)) ? 0 : 1;
}
/*
 * 对外 API：获取本组第 q_idx 题的题目 ID（101/201/.../505）。
 * 返回：题目 ID；非法下标返回 -1
 */
int game_ctx_type_question_id(const GameContext *ctx, int q_idx)
{
    if (!ctx || q_idx < 0) return -1;
    int type = judgeGroup(ctx->done_cnt);
    if (type < 0 || q_idx >= totalQs[type]) return -1;
    return (int)question_db_for_type(type)[q_idx].id;
}
/*
 * 对外 API：读取某题某选项的判定门槛、成功奖励与失败惩罚，写入 out。
 * 参数：q_idx - 题目下标；opt - 选项下标 0~3；out - 输出 OptionInfo
 * 返回：0 成功；-1 参数非法
 */
int game_ctx_type_option(const GameContext *ctx, int q_idx, int opt, OptionInfo *out)
{
    if (!ctx || !out || q_idx < 0 || opt < 0 || opt >= 4) return -1;
    int type = judgeGroup(ctx->done_cnt);
    if (type < 0 || q_idx >= totalQs[type]) return -1;
    const Option *o = &question_db_for_type(type)[q_idx].options[opt];
    out->success_attrs = o->successAttrs;
    out->success_award = o->successAward;
    out->fail_punish = o->failPunish;
    return 0;
}

/*
 * 对外 API：执行一步游戏——判定选项成败、结算属性与得分，推进上下文。
 * 参数：q_idx - 选定题目下标；opt - 选定选项 0~3；out - 输出本步结果
 * 返回：0 成功；-1 参数非法或题目已选过
 */
int game_do_step(GameContext *ctx, int q_idx, int opt, StepResult *out)
{
    if (!ctx || !out || q_idx < 0 || opt < 0 || opt >= 4) return -1;
    int type = judgeGroup(ctx->done_cnt);
    if (type < 0 || q_idx >= totalQs[type]) return -1;
    uint16_t q_bit = convert_group_mask(q_idx);
    if (ctx->group_mask & q_bit) return -1;   /* 已选过 */

    const Option *o = &question_db_for_type(type)[q_idx].options[opt];
    int ok = isSuccess(ctx->cur_attr, o->successAttrs);
    Attrs new_attr = apply_attr(ctx->cur_attr, ok ? o->successAward : o->failPunish);
    int add_score = ok ? successScore[type] : failScore[type];

    out->success = ok;
    out->add_score = add_score;
    out->new_attr = new_attr;

    /* 推进上下文 */
    ctx->cur_attr = new_attr;
    ctx->total_score += add_score;
    ctx->done_cnt += 1;
    ctx->group_mask = (mask_count(ctx->group_mask) + 1 == needPick[type]) ? 0 : (ctx->group_mask | q_bit);

    out->game_over = (ctx->done_cnt >= 10 || ctx->cur_attr.health <= 0);
    return 0;
}

/*
 * 对外 API：对某题的 4 个选项逐一做 DP 评估。
 * 每个选项输出：期望剩余总分 expect_remain；
 *               若 target_T>0 且缓存就绪，同时输出达标概率 prob_goal。
 * 参数：q_idx - 题目下标；target_T - 目标总分（0 表示不计算概率）；
 *       out - 输出 4 个选项的评估结果数组
 * 返回：0 成功；-1 参数非法
 */
int game_evaluate_options(GameContext *ctx, int q_idx, int target_T, OptionEval out[4])
{
    if (!ctx || !out || q_idx < 0) return -1;
    int type = judgeGroup(ctx->done_cnt);
    if (type < 0 || q_idx >= totalQs[type]) return -1;
    uint16_t q_bit = convert_group_mask(q_idx);
    if (ctx->group_mask & q_bit) return -1;

    const Question *q = &question_db_for_type(type)[q_idx];
    int needpick = needPick[type];

    for (int opt = 0; opt < 4; opt++) {
        const Option *o = &q->options[opt];
        int ok = isSuccess(ctx->cur_attr, o->successAttrs);
        Attrs new_attr = apply_attr(ctx->cur_attr, ok ? o->successAward : o->failPunish);
        int delta = ok ? successScore[type] : failScore[type];

        int new_done = ctx->done_cnt + 1;
        uint16_t new_mask = (mask_count(ctx->group_mask) + 1 == needpick) ? 0 : (ctx->group_mask | q_bit);

        if (ctx->expect_ready)
        {
            out[opt].expect_remain = delta +
                dpMaxAverage(&ctx->memo_expect, new_mask, new_attr, new_done);
        }
        else {
            out[opt].expect_remain = -1;
        }
        

        if (target_T > 0 && ctx->prob_ready) {
            int r = target_T - ctx->total_score - delta;
            out[opt].prob_goal =
                dpGoalProb(&ctx->memo_prob, new_mask, new_attr, new_done, r);
        } else {
            out[opt].prob_goal = -1.0;
        }
    }
    return 0;
}

int game_evaluate_options_sprint(GameContext *ctx, int q_idx,
    int completed_rounds, int total_score,
    int target, OptionEval *out4)
{
    if (!ctx || !out4) return -1;
    if (completed_rounds < 0 || completed_rounds > 6) return -1;
    // completed_rounds 0 - 6

    int type = judgeGroup(ctx->done_cnt);
    if (type < 0 || q_idx >= totalQs[type]) return -1;
    uint16_t q_bit = convert_group_mask(q_idx);
    if (ctx->group_mask & q_bit) return -1;

    static const int cumScore[11] = { 0, 6, 12, 18, 27, 36, 45, 57, 69, 81, 100 };
    int has_lost = 100 * completed_rounds + cumScore[ctx->done_cnt]
        - total_score - ctx->total_score; // 最大得分 - 当前得分 = 可丢分
    int able_lost_620 = 80 - has_lost;
    int able_lost_480 = 220 - has_lost;
    /* -----------------------------------------------------------------
     * TODO(内核)：
     *   - 用 ctx 的局内进度（done_cnt / score / attrs）折算当前剩余
     *     "可丢分预算"，等价于把 dpGlobalProMax 的 able_lost_s 还原出来；
     *   - target == 620 -> 以 win_rates80  为跨局边界做一遍选项 DP；
     *     target == 480 -> 以 win_rates220 为跨局边界做一遍选项 DP；
     *     target == 0   -> 走现有 game_evaluate_options(ctx, q_idx, 0, out4)；
     *   - out4[i].expect_remain 语义不变（本局剩余期望得分）；
     *     out4[i].prob_goal     = 选择该选项后"最终达标 target"的概率。
     * ---------------------------------------------------------------- */

     // typedef struct {
     //     double expect_remain; /* 选该选项后的"剩余期望得分" */
     //     double prob_goal;     /* 选该选项后的"达到目标 T 的概率"，未启用时为 -1 */
     // } OptionEval;
    const Question *q = &question_db_for_type(type)[q_idx];
    int needpick = needPick[type];

    for (int opt = 0; opt < 4; opt++)
    {
        const Option *o = &q->options[opt];
        int ok = isSuccess(ctx->cur_attr, o->successAttrs);
        Attrs new_attr = apply_attr(ctx->cur_attr, ok ? o->successAward : o->failPunish);
        int delta = ok ? successScore[type] : failScore[type];
        int delta2 = ok ? 0 : successScore[type] - failScore[type];

        int new_done = ctx->done_cnt + 1;
        uint16_t new_mask = (mask_count(ctx->group_mask) + 1 == needpick) ? 0 : (ctx->group_mask | q_bit);

        if (ctx->expect_ready) // 注意未开启期望计算，调用dpMaxAverage会解引用空的哈希表，增加防御
        {
            out4[opt].expect_remain = delta +
                dpMaxAverage(&ctx->memo_expect, new_mask, new_attr, new_done);
        }
        else
        {
            out4[opt].expect_remain = -1;
        }

        if (target == 480 && ctx->proMax480_ready) {
            out4[opt].prob_goal =
                dpGlobalProMax(&ctx->memo_proMax480, new_mask, new_attr, new_done,
                    able_lost_480 - delta2, completed_rounds, winrate220, 221);
        }
        else if (target == 620 && ctx->proMax620_ready) {
            out4[opt].prob_goal =
                dpGlobalProMax(&ctx->memo_proMax620, new_mask, new_attr, new_done,
                    able_lost_620 - delta2, completed_rounds, winrate80, 81);
        }
        else {
            out4[opt].prob_goal = -1.0;
        }
    }

    return 0;
}



/*
 * 对外 API：查询从当前状态出发的期望剩余总分。
 * 先查缓存，未命中则调用 dpMaxAverage 计算并标记缓存为脏。
 * 返回：期望剩余分；缓存未就绪返回 -1.0
 */
double game_query_expect(GameContext *ctx)
{
    if (!ctx || !ctx->expect_ready) return -1.0;

    uint64_t key = encode_flat_key(ctx->group_mask, ctx->cur_attr,
                                   (uint8_t)ctx->done_cnt, NO_REMAIN_AIM_SCORE);
    HashEntry *e = memo_hash_lookup(&ctx->memo_expect, key);
    if (e != NULL) return e->val;

    ctx->dirty_expect = TRUE;
    return dpMaxAverage(&ctx->memo_expect, ctx->group_mask, ctx->cur_attr,
                        ctx->done_cnt);
}

/*
 * 对外 API：查询从当前状态出发，最终总分 >= T 的概率。
 * 先查缓存，未命中则调用 dpGoalProb 计算并标记缓存为脏。
 * 参数：T - 目标总分
 * 返回：达标概率；缓存未就绪返回 -1.0
 */
double game_query_goal_prob(GameContext *ctx, int T)
{
    if (!ctx || !ctx->prob_ready) return -1.0;
    int r = T - ctx->total_score;
    if (r < 0) r = 0;

    uint64_t key = encode_flat_key(ctx->group_mask, ctx->cur_attr,
                                   (uint8_t)ctx->done_cnt, (uint8_t)r);
    HashEntry *e = memo_hash_lookup(&ctx->memo_prob, key);
    if (e != NULL) return e->val;

    // ctx->dirty_prob = TRUE;
    return dpGoalProb(&ctx->memo_prob, ctx->group_mask, ctx->cur_attr,
        ctx->done_cnt, r);
}

#ifdef DEBUG
/*
 * 对外 API：获取 memo 缓存命中 / 未命中计数。
 * 只统计已被 memo 缓存的 DP 子问题（mask, attr, done_cnt, r）；
 * 命中计数由 dpCal.c 直接写入 ctx->stats，读取时与当前状态无关。
 * 返回：DebugStats 只读指针；ctx 为 NULL 返回 NULL
 */
const DebugStats* game_ctx_get_stats(const GameContext *ctx)
{
    if (!ctx) return NULL;
    return (const DebugStats *)&ctx->stats;
}
#endif

/*
 * 对外 API：根据题目 ID 在当前阶段题库中反查本组内下标。
 * 参数：question_id - 题目 ID（如 201、305）
 * 返回：本组下标；未找到返回 -1
 */
int game_ctx_find_qidx_by_id(const GameContext *ctx, int question_id)
{
    int type = judgeGroup(ctx->done_cnt);
    if (type < 0) return -1;
    const Question *db = question_db_for_type(type);
    for (int i = 0; i < totalQs[type]; i++) {
        if (db[i].id == question_id) return i;
    }
    return -1;
}
