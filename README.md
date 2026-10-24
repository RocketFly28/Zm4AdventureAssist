# 生存大冒险 · 策略求解内核

**Survival Adventure — an exact DP solver that maximizes your chance of hitting the 620-point reward tier.**

> 把「靠运气的冒险」算成「有最优解的最优化问题」：给定角色属性和答题进度，
> 精确算出每个选项能把「达标概率」推到多高。

一个用 C99 写的纯计算内核，无第三方依赖，可编译为：

- **原生控制台程序**（Windows / MSVC 或 gcc）
- **静态库 / 动态库**，供其它语言调用
- **WebAssembly**（Emscripten），已在微信小程序与网页中跑通

---

## 背景

因为对应的微信小程序备案太费劲，作者懒得弄了。所以把内核代码扔上来。

《生存大冒险》是《造梦西游4》内置的一个小游戏：每个阶段给出一批题目，玩家从每题的
4 个选项里挑一个，系统按当前 6 维属性判定成功 / 失败，成功加分，失败扣分甚至扣血，
血归零即出局。

这个玩法的奖励只认最终总分——**连续 7 局打完、总分达到 620（满分 700）才能拿到最高一档
奖励**，差一分就掉档。所以真正的目标不是"每一步都拿分"，而是：

> **让走完全程后总分 ≥ 620 的概率最大。**

本程序用动态规划**精确求解**这个概率——不是蒙特卡洛模拟，不是贪心，也不是经验攻略，
而是把整棵决策树上的所有分支都算出来，对每道题的 4 个选项给出各自的达标概率，
取最高的那个。同时它还会算出 **总分 ≥ 480 的概率**：

- `P(≥620)` 是主目标，对应最高额奖励；
- `P(≥480)` 是**保底策略**——当 620 已经不可能、或者强行冲 620 会把胜率压得很低时，
  按 480 这一档的最优策略走，至少把保底奖励拿稳。

这也是为什么内核里有两张概率表（`win_rates80` / `win_rates220`）：
620 分档允许全程丢 80 分，480 分档允许丢 220 分，两档的最优策略并不相同。

手工做攻略表只能穷举出几条路线，而这里的答案是**精确解**。

### 它能回答什么

| 你想知道 | 调用 |
|---|---|
| 现在这一步，4 个选项哪个最好？ | `game_evaluate_options` / `wasm_sprint_evaluate` |
| 冲 620 的把握还有多大？ | `wasm_sprint_winrate`（查预计算表，零开销） |
| 保底 480 还稳不稳？ | 同上，第二个返回值 |
| 不管奖励、只想把期望分数拉满呢？ | `game_query_expect` / `dpMaxAverage` |

内核不含任何 UI、IO 依赖和全局可变状态：所有状态封在 `GameContext` 里，
DP 只通过指针参数访问缓存表，因此**可重入**，同一进程可以同时跑多局
（后端多会话、WASM 单线程多实例都能用）。

---

## 目录

- [背景](#背景)
- [游戏规则（内核中的模型）](#游戏规则内核中的模型)
- [算法](#算法)
- [文件说明](#文件说明)
- [编译与运行](#编译与运行)
- [命令行用法](#命令行用法)
- [预计算打表（PRE_CAL）](#预计算打表pre_cal)
- [C API 速览](#c-api-速览)
- [WebAssembly 导出接口](#webassembly-导出接口)
- [已知限制与注意事项](#已知限制与注意事项)
- [License](#license)

---

## 游戏规则（内核中的模型）

**一局 = 5 个阶段，共 10 道题，满分 100 分。连续 7 局为一次完整挑战，总分 700。**

| 阶段 | 题库数量 | 本阶段需选题数 | 单题成功得分 | 单题失败得分 |
|:---:|:---:|:---:|:---:|:---:|
| 1 | 4  | 1 | 6  | 3 |
| 2 | 10 | 2 | 6  | 3 |
| 3 | 15 | 3 | 9  | 4 |
| 4 | 7  | 3 | 12 | 6 |
| 5 | 5  | 1 | 19 | 9 |

题库共 **41 道题**（ID 形如 `101` / `205` / `315`，百位即阶段号），题目与规则数据以
`static const` 表的形式内嵌在 `Adventure.c`，无运行时解析开销。

**6 维属性**：健康 `health`、战力 `power`、智慧 `wisdom`、魅力 `charm`、运气 `luck`、灵巧 `agility`。
除健康被钳制在 `1..10` 外，其余各维在 `0..12` 处截断（`PWCLA_MAX`）。

**判定**：每个选项带一个 `successAttrs` 门槛，当前属性**逐维**都 ≥ 门槛即成功；
成功按 `successAward` 增减属性，失败按 `failPunish` 增减属性。

**死亡**：`health <= 0` 时角色出局，本局剩余题目的分数**全部作废**
（DP 中直接把剩余满分从预算里扣掉，这是有意的不对称设计）。
正常答完 10 题则不扣。

**两档奖励目标**：

| 档位 | 目标总分 | 全程最多可丢 |
|---|---|---|
| 最高额 | 620 / 700 | 80 分 |
| 保底 | 480 / 700 | 220 分 |

---

## 算法

### 状态空间

一个 DP 子问题由四元组唯一确定：

```
( group_mask, 属性向量, done_cnt, able_lost_s )
   ↑ 本阶段已选题目位图   ↑ 6 维   ↑ 已完成题数   ↑ 剩余可丢分预算
```

- `group_mask` 用 `uint16_t` 位图记录**当前阶段**已选过的题（阶段切换时清零）；
- `done_cnt ∈ [0, 10]`，同时决定当前处于哪个阶段（`judgeGroup`）；
- `able_lost_s` 是"还能丢多少分"，也就是距离目标分数还剩多少容错空间。

### 状态压缩

整个四元组被打包进一个 `uint64_t`，作为哈希表的 key：

| 位段 | 含义 | 位宽 |
|---|---|---|
| `[0-15]`  | `group_mask` | 16 |
| `[16-19]` | `health - 1` | 4 |
| `[20-23]` | `power` | 4 |
| `[24-27]` | `wisdom` | 4 |
| `[28-31]` | `charm` | 4 |
| `[32-35]` | `luck` | 4 |
| `[36-39]` | `agility` | 4 |
| `[40-43]` | `done_cnt` | 4 |
| `[44-51]` | `able_lost_s` | 8 |

`health <= 0` 直接返回 `UINT64_MAX` 作为"无效状态"哨兵，天然不会被缓存。

### 记忆化

`hash.c` 是一个**开放寻址哈希表**（线性探测）：key 的最高位 `bit63` 兼作"槽位已占用"标记，
装载因子 ≥ 0.7 自动翻倍扩容，支持 `memo_hash_compress` 把稀疏表蒸馏成紧凑表。
哈希函数为 Murmur3 风格的 64→32 位混合。

缓存还可以**落盘复用**：`init_<role>_expect.cache`，文件头带 `MEMO` magic + 版本号 + 角色号校验，
不匹配即判定损坏并重算。

### 剪枝

`prune_options` 在展开子问题前先砍掉两类选项，直接把分支因子从 4 压下去：

1. **帕累托支配**：若选项 B 的六维属性与成功标记**全都不劣于** A，则 A 被支配，删掉。
2. **必死且非最后一题**：结算后 `health <= 0` 而仍有后续题目的选项，直接删掉。

### 两个递推

**期望分**（`dpMaxAverage`）——对剩余每道题取「最优选项」，再对题目集合取平均：

```
V(s) = (1 / 剩余题数) · Σ_题 Σ_选项 max( 本题得分 + V(s') )
```

**达标概率**（`dpGlobalProMax`）——多了"局"这一维，按局**反向**递推：

```
P(局, s) = 对剩余题目取平均 · 每题取 4 个选项里概率最高的一个
终局时查已经算好的下一局概率表（win_rates[game_round][able_lost_s]）
```

计算结果就是 `win_rates80[6][81]` 与 `win_rates220[6][221]` 两张表
（6 个局号 × 81 / 221 个可丢分档位）。这两张表**一次算完、固化成常量**，
运行时只查表，**零 DP 开销**。

### 预计算并行化

`init_win_rate` 用 OpenMP 按「(角色, 可丢分档位)」任务队列切分：
每个线程独立持有一份 memo 表（避免锁竞争），最后用一次 `critical` 合并到结果表。

---

## 文件说明

仓库只包含源码，共 12 个文件，平铺在同一目录即可编译（互相只通过头文件依赖）：

| 文件 | 职责 |
|---|---|
| `common.h` | 编译开关（`WASM` / `PRE_CAL` / `DEBUG`）与游戏版本号 |
| `Adventure.h` / `.c` | 游戏状态机、题库、规则常量、纯工具函数、查询接口、磁盘缓存 IO |
| `dpCal.h` / `.c` | DP 本体：期望分、达标概率、多局全局最优、打表门面函数 |
| `hash.h` / `hash.c` | 开放寻址 memo 哈希表 |
| `cli.h` / `cli.c` | 命令行前端：子命令、REPL、输出格式化；只调用公开 API |
| `main.c` | 进程入口，只把 `argc/argv` 转给 `cli_main` |
| `wasm_api.h` / `.c` | Emscripten 导出层，与内核解耦；不编 wasm 时可以不参与编译 |

内核三件套（`Adventure`、`dpCal`、`hash`）**不含任何 `main`，也不碰 stdin/stdout**，
可以直接当静态库链接进别的宿主；命令行只是它的一个使用者。

仓库里**没有**工程文件、构建产物、题目数据的 JSON / CSV 副本，也没有打包脚本——
这些都属于本机环境，换台机器重建即可。

---

## 编译与运行

### gcc / clang

```bash
gcc -std=gnu11 -O2 -fopenmp Adventure.c dpCal.c hash.c cli.c main.c -o adventure
./adventure solve --role 3 --target 620
```

`-fopenmp` 只有打表（`PRE_CAL`）时才真正需要，纯查表运行可以去掉。

只想编成**不含 main 的内核库**：不把 `cli.c` / `main.c` 加进编译即可。

```bash
gcc -std=gnu11 -O2 -c Adventure.c dpCal.c hash.c
ar rcs libadventure.a Adventure.o dpCal.o hash.o
```

### Windows / MSVC

新建一个空的控制台项目，把这 12 个文件全部加进去（Debug/Release x64 都行），
或者直接用命令行：

```bat
cl /O2 /openmp Adventure.c dpCal.c hash.c cli.c main.c
```

> 代码要**同时在 MSVC、gcc、emcc 下编译**。某些 gcc 能接受的写法 VS 会直接报错，
> 例如 `#pragma omp for` 的循环变量**必须在 `for` 外声明**——改代码时按 MSVC 的限制来写。

### WebAssembly / Emscripten

```bash
emcc Adventure.c dpCal.c hash.c wasm_api.c -O3 --no-entry -fcommon -DWASM \
  -sMODULARIZE=1 -sEXPORT_NAME=createAdventureModule \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=256MB -sMAXIMUM_MEMORY=2GB \
  -sFORCE_FILESYSTEM=1 \
  -sEXPORTED_FUNCTIONS=_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,getValue,setValue,HEAP8,HEAPU8,HEAPF64,FS \
  -o adventure.js
```

`-DWASM` 必须带上：默认是原生控制台构建，加上它才会关掉控制台相关代码、
不编 `main.c` / `cli.c`。（`--no-entry` 保证不产生入口。）

产出 `adventure.js` + `adventure.wasm`，用 `createAdventureModule()` 加载，
之后按下面的导出接口调用。

> 若要跑在**小程序**里，还需要把 Emscripten 胶水中的加载器换成 `WXWebAssembly`，
> 并把可选链、空值合并、逻辑赋值、class 字段等 ES2020+ 语法降级
> （部分真机 JS 引擎会直接 SyntaxError）。这部分适配脚本属于宿主环境，未包含在本仓库。

---

## 命令行用法

编译出的 `adventure` 有两种模式：**argv 子命令**，以及不带子命令时进入的**交互式 REPL**。
两种模式都只调用 `Adventure.h` 的公开接口，不碰内核内部字段。

### argv 模式

```bash
./adventure solve --role 3 --target 620
```

| 选项 | 含义 | 默认 |
|---|---|---|
| `--role N` | 角色编号 1~7 | 3 |
| `--target T` | 目标总分；`620` / `480` 走多局冲刺模型，其余走单局模型 | 620 |
| `--round N` | 已完成局数 0~6 | 0 |
| `--score S` | 之前几局累计得分 | 0 |
| `--quiet` | 只打印总结 | 关 |

`solve` 会把本局一路打到结束，每一步打印它认为最优的题目与选项，
以及该步之后的达标概率与剩余期望分：

```
step  1 | stage 1 | q_idx  0 (qid 101) | opt 2 | P  3.121% | expect    92.44 | total   0
         -> SUCCESS +6, hp=3 pow=4 wis=5 cha=5 luck=4 agi=3
...
Round score : 97
Total score : 97  (prior 0 + this round 97)
Progress    : round 1 of 7 finished
Success P   :   3.121% (estimated from the first step)
```

其他子命令：

| 命令 | 作用 |
|---|---|
| `adventure roles` | 列出 7 个角色及初始属性 |
| `adventure rules` | 五个阶段的题目数、必选数、成功/失败得分 |
| `adventure table [--out DIR]` | 打表（仅 `PRE_CAL` 构建可用） |
| `adventure version` | 版本与构建模式 |
| `adventure help` | 用法 |

### REPL 模式

直接运行 `./adventure` 进入：

| 命令 | 作用 |
|---|---|
| `role N` / `target T` / `round N` / `score S` | 改设置（前三个会重开一局） |
| `status` | 当前属性、得分，以及此刻的最优选择 |
| `q` | 列出本阶段还能选的题 |
| `opt Q` | 列出第 Q 题的四个选项，及各选项的达标概率 |
| `pick Q O` | 用第 O 个选项回答第 Q 题 |
| `auto` | 剩下的题全部自动最优推进 |
| `reset` / `help` / `quit` | 重开 / 帮助 / 退出 |

> 提示语全部是纯 ASCII，避免在非 Windows 终端（UTF-8）上出现乱码。

---

## 预计算打表（PRE_CAL）

概率表默认是 `dpCal.c` 的 `#else` 分支里那两张**已算好并固化的常量表**。
如果要重新生成（例如改了规则），走 `PRE_CAL` 流程。
打表是**离线一次性的重活**：状态空间大、递归深，8 线程跑完需要耐心。

1. 取消注释 `common.h` 里的 `#define PRE_CAL`（或编译加 `-DPRE_CAL`）
2. 编译，然后运行 `./adventure table --out .`
   （内部依次算 80 分档与 220 分档，并把结果写进 `winRate80.txt` / `winRate220.txt`）
3. 把两个文本的内容分别粘贴进 `dpCal.c` 的 `#else` 分支
   （`win_rates80` / `win_rates220` 两处静态常量表）
4. 关掉 `PRE_CAL`，重新编译——之后启动即查表，没有任何 DP 计算

---

## C API 速览

`Adventure.h` 暴露的接口大致分四组（完整声明见头文件）：

| 分组 | 函数 | 说明 |
|---|---|---|
| 生命周期 | `game_ctx_create` / `game_ctx_create_form` | 开一局；后者从局面快照续档 |
| | `game_ctx_create_sprint` / `_sprint_form` | 冲刺模式（7 局） |
| | `game_ctx_modify` | 用快照原地改写当前局面 |
| | `game_ctx_destroy` | 结束一局，脏缓存自动落盘 |
| 只读查询 | `game_ctx_get_attrs` / `_done_cnt` / `_total_score` | 当前属性 / 进度 / 累计分 |
| | `game_ctx_get_type` / `_mask` | 当前阶段、已选题位图 |
| | `game_ctx_type_total` / `_need` / `_available` / `_question_id` / `_option` | 当前阶段的题目信息 |
| | `game_ctx_find_qidx_by_id` | 按题目 ID 反查下标 |
| 执行 | `game_do_step` | 选某题某选项，推进状态，返回判定结果 |
| DP 查询 | `game_evaluate_options` | 评估某题 **4 个选项**各自的期望分与达标概率（推荐用） |
| | `game_evaluate_options_sprint` | 冲刺模式下的选项评估（指定 620 / 480 目标） |
| | `game_query_expect` | 当前局的剩余期望最高分 |
| | `game_query_goal_prob` | 当前局达到目标 T 的概率 |

典型调用顺序：

```c
GameContext *ctx = game_ctx_create(ROLE_XIAO_TANG);

int total = game_ctx_type_total(ctx);
for (int q = 0; q < total; q++) {
    if (!game_ctx_type_available(ctx, q)) continue;

    OptionEval ev[4];
    game_evaluate_options(ctx, q, /*target_T=*/82, ev);   /* ev[i].expect_remain / prob_goal */

    /* 挑一个 → 推进 */
    StepResult sr;
    game_do_step(ctx, q, best_opt, &sr);
}

game_ctx_destroy(ctx);
```

---

## WebAssembly 导出接口

`wasm_api.c` 把上面的 C API 压平成 wasm 友好的一层（全部 `extern "C"`）：

- 基础：`wasm_game_create` / `wasm_game_destroy` / `wasm_game_do_step` / `wasm_game_evaluate`
- 快照续档：`wasm_game_create_form` / `wasm_game_ctx_modify` /
  `wasm_game_snapshot_fill` / `wasm_game_snapshot_size`
  （`GameSnapshot` 的内存布局在 `Adventure.h` 里逐字节写明，JS 侧不用手算偏移）
- 冲刺：`wasm_sprint_winrate`（纯函数查表）/ `wasm_sprint_evaluate`
- 版本：`wasm_abi_version()`，前端启动时校验，避免 wasm 与页面版本错配

JS 侧用 `_malloc` / `_free` 分配 `OptionEval[4]`（64 字节）等结构体，
通过 `HEAPF64` 或 `Module.getValue(ptr, "double")` 读结果。

---

## 已知限制与注意事项

- **编译器以 MSVC 为准**。代码要同时在 MSVC、gcc、emcc 下编译；`#pragma omp for` 的
  循环变量必须在 `for` 外声明，否则 VS 直接报错。
- **key 的位宽是硬约束**。`able_lost_s` 在 key 里只占 8 bit（最大 255），
  当前的 80 / 220 两档都在范围内；若将来放宽丢分上限，必须同步扩位，否则会 key 撞车。
- **默认是原生控制台构建**。编 WebAssembly 时必须显式加 `-DWASM`，
  否则会去编 `main.c` / `cli.c`；反过来只想出库文件时，把这两个文件排除即可。
- **源码编码为 GBK**（含中文注释），换行统一为 CRLF；只有 `cli.c` / `cli.h` / `main.c`
  是纯 ASCII（注释也是英文），不受代码页影响。
  用 VS 打开可直接识别；gcc 需要 `-finput-charset=GBK -fexec-charset=GBK`，
  否则中文注释会乱码（不影响编译结果）。
- **题库数据内嵌**。改题目要改 `Adventure.c` 里的 `static const` 表，
  规则常量（`totalQs` / `needPick` / `successScore` / `failScore`）在同文件，注意保持一致。
- **memo 表很吃内存**。打表时每线程一份 `1 << 24` 槽的表（每槽 16 字节 ≈ 256 MB），
  8 线程约 2 GB；运行期每份 `1 << 20` 槽 ≈ 16 MB。
  机器小就调小初始容量，靠 0.7 装载因子自动扩容。
- **DP 是递归实现**，栈深度等于题目数（10），安全。

---

## License

[MIT](LICENSE) © 2026 RocketFly28

随便用：可以复制、修改、合并、发布、商用、再授权，只要保留版权声明和许可声明。
软件按「原样」提供，作者不承担任何担保或责任。

> 说明：MIT 覆盖的是本仓库的**代码**。《造梦西游4》及《生存大冒险》的游戏内容、
> 题库规则与名称，版权归原权利方所有，不在本许可范围内。
