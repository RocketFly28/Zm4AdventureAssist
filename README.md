# 生存大冒险 · 策略求解内核

**Survival Adventure — an exact DP solver, plus a micro Transformer policy that answers in one forward pass.**

> 把「靠运气的冒险」算成「有最优解的最优化问题」：给定角色属性和答题进度，
> 精确算出每个选项能把「达标概率」推到多高；或者用一个 6 MB 的小网络，
> 一次前向推理直接给出答案。

一个用 C99 写的纯计算内核，无第三方依赖，可编译为：

- **原生控制台程序**（Windows / MSVC 或 gcc）
- **静态库 / 动态库**，供其它语言调用
- **WebAssembly**（Emscripten），已在微信小程序与网页中跑通

内核提供两套独立的决策引擎，命令行可随时切换：

| 模式 | 引擎 | 特点 |
|---|---|---|
| **flash**（默认） | 27-token Transformer 策略网络 | 一次前向，微秒级，内存恒定；统计意义上逼近 DP |
| **precision** | 精确动态规划 + 记忆化 | 数学精确解；每步要展开大量子问题 |

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

Transformer 网络没有蒸馏 DP，它是作者没事闲的，采用MCTS从zero训练出的。最终p-head的直觉已经接近DP解。
输入当前局面，直接输出 4 个选项的偏好与价值，单次前向即可决策。
考虑到移动端在DP加载时多少有些缓慢，因此把它加入到项目中，相当于快速地给出一个 DP 的近似策略。

**flash 给速度和体积，precision 给正确答案**，两者在同一份代码里共存、可交叉验证。

### 它能回答什么

| 你想知道 | 调用 |
|---|---|
| 现在这一步，4 个选项哪个最好？（精确） | `game_evaluate_options` / `wasm_game_evaluate` |
| 现在这一步，4 个选项哪个最好？（快，默认） | `flash_evaluate_options` / `wasm_game_evaluate_flash` |
| 冲 620 的把握还有多大？ | `game_evaluate_options_sprint`（查预计算表） |
| 保底 480 还稳不稳？ | 同上，`target` 传 480 |
| 不管奖励、只想把期望分数拉满呢？ | `game_query_expect` / `dpMaxAverage` |
| flash 和精确解差多少？ | REPL 的 `disparity` 命令 |

内核不含任何 UI、IO 依赖和全局可变状态：所有状态封在 `GameContext` 里，
DP 只通过指针参数访问缓存表，因此**可重入**，同一进程可以同时跑多局
（后端多会话、WASM 单线程多实例都能用）。

> flash 模式的推理函数虽然用了几块文件级 `static` 缓冲（省掉每次前向的 malloc），
> 但那份缓冲只在 `net.c` 内部可见，且写入-读取在同一函数内完成；多局共享同一个
> `GameContext` 语义不受影响。要做真并发推理则需给 `net.c` 的缓冲加锁或改成栈分配。

---

## 目录

- [背景](#背景)
- [两种模式](#两种模式)
- [游戏规则（内核中的模型）](#游戏规则内核中的模型)
- [精确求解：DP 与记忆化](#精确求解dp-与记忆化)
- [flash：蒸馏出来的策略网络](#flash蒸馏出来的策略网络)
- [文件说明](#文件说明)
- [编译与运行](#编译与运行)
- [命令行用法](#命令行用法)
- [预计算打表（PRE_CAL）](#预计算打表pre_cal)
- [C API 速览](#c-api-速览)
- [WebAssembly 导出接口](#webassembly-导出接口)
- [已知限制与注意事项](#已知限制与注意事项)
- [License](#license)

---

## 两种模式

同一份内核、同一个 `GameContext`，两条决策路径：

```
                       ┌──────────────────────────────────────┐
  当前局面 ──────────► │ precision: dpCal.c                   │
  (mask, attrs,        │   prune → 展开子问题 → memo 查表      │──► 精确 P / 期望分
   done_cnt, budget)   │   每步可能几十万个子问题              │
                       └──────────────────────────────────────┘
                       ┌──────────────────────────────────────┐
                 ────► │ flash: net.c                         │
                       │   202 维特征 → 27 token → Transformer │──► argmax p + V
                       │   一次前向，无搜索                    │
                       └──────────────────────────────────────┘
```

- **precision** 的输出是**数学精确值**：在给定模型（规则、概率、目标）下，这个选项
  确实是让达标概率最大的那个。代价是每步都要做一次带剪枝和记忆化的搜索，
  首次运行还要建哈希表。
- **flash** 的输出是**模型预测**：一个训练过的策略网络，直接从局面映射到动作。
  它不保证最优，但在实测统计上非常接近。

命令行默认走 **flash**（`CliOptions.use_nn` 初值为 1），用 `--precision` 切回 DP。
REPL 里对应 `flash` / `precision` 两个命令，`disparity` 会把两者的实测差距打出来。

> flash 模式忽略 `--target`：网络是在 `target_round = 70`（单局 70 分口径）下训练的，
> 不区分 620 / 480 两档。要按档位优化就必须用 precision。

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
`static const` 表的形式内嵌在 `src/dpsolver/Adventure.c`，无运行时解析开销。

**6 维属性**：健康 `health`、战力 `power`、智慧 `wisdom`、魅力 `charm`、运气 `luck`、灵巧 `agility`。
除健康被钳制在 `1..10` 外，其余各维在 `0..12` 处截断（`PWCLA_MAX`）。

**判定**：每个选项带一个 `successAttrs` 门槛，当前属性**逐维**都 ≥ 门槛即成功；
成功按 `successAward` 增减属性，失败按 `failPunish` 增减属性。
门槛填 `health = 127` 是"该选项永远不可能成功"的哨兵。

**死亡**：`health <= 0` 时角色出局，本局剩余题目的分数**全部作废**
（DP 中直接把剩余满分从预算里扣掉，这是有意的不对称设计）。
正常答完 10 题则不扣。

**两档奖励目标**：

| 档位 | 目标总分 | 全程最多可丢 |
|---|---|---|
| 最高额 | 620 / 700 | 80 分 |
| 保底 | 480 / 700 | 220 分 |

---

## 精确求解：DP 与记忆化

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

整个四元组被打包进一个 `uint64_t`，作为哈希表的 key（`encode_flat_key`）：

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

`dpGoalProb` 现在只是一层薄适配：把"还差分值 r"换算成 `able_lost_s` 后转发给
`dpGlobalProMax`（`game_round` 取 `MAX_ROUND`，不需要查表）。

### 预计算并行化

`init_win_rate` 用 OpenMP 按「(角色, 可丢分档位)」任务队列切分：
每个线程独立持有一份 memo 表（避免锁竞争），最后用一次 `critical` 合并到结果表。

---

## flash：微型策略网络

`src/net/` 是一个不依赖任何推理框架的**手写前向实现**——没有 ONNX、没有 BLAS，
只有 `ops.h` 里几个朴素循环算子（`linear` / `relu_inplace` / `layernorm` /
`softmax_inplace` / `add_vec` / `embedding_lookup`），权重以 C 数组形式编译进二进制。

### 输入：202 维特征

`net_api.c` 负责把 `GameContext` 的当前局面展开成一条 202 维 `float` 向量。
这个布局是**硬约定**，必须与训练侧（Python 的 `features.py` / `norm.py`）逐项对齐：

| 区间 | 维度 | 内容 |
|---|---|---|
| `[0, 41)` | 41 | 当前题 one-hot（全局题号 0..40） |
| `[41, 65)` | 24 | 4 选项 × 6 维门槛 |
| `[65, 93)` | 28 | 4 选项 × [成功得分, 成功增益 6 维] |
| `[93, 121)` | 28 | 4 选项 × [失败得分, 失败惩罚 6 维] |
| `[121]` | 1 | 当前生命 |
| `[122, 127)` | 5 | power / wisdom / charm / luck / agility |
| `[127, 131)` | 4 | `round_sc` / `score_sc` / `target_sc` / `remain_round_sc` |
| `[131]` | 1 | 已答题数 / 10 |
| `[132, 167)` | 35 | `arch_need`：5 类主属性 × [剩余占比, need 均值 6 维] |
| `[167, 202)` | 35 | `arch_gain`：5 类主属性 × [剩余占比, 增益均值 6 维] |

量纲口径（与 Python 侧 `norm.py` / `config.py` 一致）：

- 属性一律 clip 到 `[0, 12]` 再 `/ 12`；
- 门槛哨兵 127 压到 13，再 clip 到 `[0, 13]`；
- 分数、属性增益**不归一**，用原始值；
- `score_sc = 累计分 / (100 × 总局数)`，训练时总局数 = 1；
- `target_sc = 70 / 100`；
- `round_sc` / `remain_round_sc` 在单局口径下恒为 0。

`arch_*` 是"剩余题库画像"：对还没出现的题目按**主属性**（门槛 6 维里最大的那一维，
health 不参评）聚合成 5 类，每类给出"还剩多少题"和"平均要什么 / 给什么"。
这样"第 10 题可能需要智慧 7"这类信息就不必靠网络从历史里硬记。

> 聚合口径有一处容易踩坑：必须**先按题内成员求均值，再按题累加**。
> 一道题若有两个选项同属一个主属性，两种算法结果不同（题库里有 6/41 道题是这种情况）。

### 网络结构

```
202 维 ──► 10 个输入投影（权重共享到 27 个 token）
        ──► + type embedding（11 种 token 类型）
        ──► 1 层 Transformer encoder
               pre-LN ─► MHSA（d_model=128, 4 head × 32）─► 残差
                      ─► LN ─► FFN 128→256(ReLU)→128 ─► 残差
        ──► 聚合：15 个全局 token 展平 + 12 个选项 token 的 mean / max
               = 17 × 128 = 2176
        ┌─► state_mlp 2176→128 (ReLU) ─► h_state
        ├─► value head  128→64(ReLU)→1 ─► sigmoid ─► V（预测本局得分 / 100）
        └─► policy head ×4：concat(needᵢ, succᵢ, failᵢ, h_state) = 4×128 → 64(ReLU) → 1
                                          ─► softmax(4) ─► p[4]
```

各 token 的分工（与 `net.c` 的 `GROUP_MAP` 一致）：

| 组 | token 数 | 维度 | 含义 |
|---|:---:|:---:|---|
| `q` | 1 | 41 | 当前题 one-hot |
| `need` | 4 | 6 | 各选项门槛 |
| `succ` | 4 | 7 | 各选项成功得分 + 增益 |
| `fail` | 4 | 7 | 各选项失败得分 + 惩罚 |
| `health` | 1 | 1 | 生命 |
| `other` | 1 | 5 | 其余五维 |
| `round` | 1 | 4 | 局数 / 分数 / 目标 / 剩余比例 |
| `step` | 1 | 1 | 已答题数 |
| `arch_need` | 5 | 7 | 未来需求画像 |
| `arch_gain` | 5 | 7 | 未来收益画像 |

合计 **27 token**、`d_model = 128`。所有位置索引都通过 `IDX[...]` / `TOKEN_TYPES`
查表取出，`net_forward` 里不写死下标，改 token 排布时不会静默错位。

### 权重

`src/net/weight/weights.h` 是从 PyTorch `state_dict` 直接导出的 C 数组，
约 6.3 MB / 5.8 万行，全部 `static const float`：

- `projs_{0..9}_{weight,bias}` — 10 个输入投影
- `encoder_layers_0_{norm1,norm2,self_attn.*,linear1,linear2}_{weight,bias}` — 那一层 encoder
- `type_emb_weight` — token 类型嵌入
- `state_mlp_0_*` / `v_head_0_*` / `v_head_2_*` / `p_head_0_*` / `p_head_2_*`

导出时保持 PyTorch 的 `[out, in]` 排列，所以 `linear()` 里的下标是
`W[i * in_dim + j]`——**不要**按 `[in, out]` 去转置。

### 效果

REPL 的 `disparity` 内嵌了一组实测数据（1000 局，角色随机，`seed = 42`）：

```
           [PK] 1000 games  role = random  seed=42
     mode          |   avg  |   std  |  min  |  max  |  >=70% | death%
-------------------+--------+--------+-------+-------+--------+--------
 Flash(NN-net)     |  76.8  |  12.0  |   36  |  100  |   92%  |   60%
 Precision(DP)     |  77.3  |  12.3  |   41  |  100  |   91%  |   68%
```

平均分只差 0.4 分，而 flash 的死亡率和方差都略低（网络学到了一点"保守化"的倾向）。
注意这不是证明——它只是一组固定种子的抽样，换成别的种子数字会动。

---

## 文件说明

仓库只包含源码，按功能分目录，互相只通过头文件依赖：

```
src/
├── main.c                    进程入口，只把 argc/argv 转给 cli_main
├── dpsolver/                 精确求解内核（无 main、不碰 stdio）
│   ├── common.h              编译开关（WASM / PRE_CAL / DEBUG）与游戏版本号
│   ├── Adventure.h/.c        状态机、题库、规则常量、纯工具函数、查询接口、磁盘缓存
│   ├── dpCal.h/.c            DP 本体：期望分、达标概率、多局全局最优、打表门面
│   └── hash.h/.c             开放寻址 memo 哈希表
├── cli/                      控制台前端
│   ├── cli.h/.c              子命令、REPL、输出格式化；只调用公开 API
│   └── guide_table.h         41 题 × 4 选项的英文名（纯显示用，判定数据仍走内核）
├── net/                      flash 模式：神经网络推理（零第三方依赖）
│   ├── net.h/.c              前向传播：投影 → Transformer → state MLP → 双头
│   ├── net_api.h/.c          202 维特征构造 + flash_evaluate_options
│   ├── ops.h                 手写算子（linear / relu / layernorm / softmax / …）
│   └── weight/weights.h      导出的 PyTorch 权重（≈6.3 MB）
└── wasm/                     Emscripten 导出层，原生构建不参与编译
    └── wasm_api.h/.c
```

另有 `Makefile`（GCC / MinGW 构建）、`LICENSE`、`README.md`。

分层关系：

- **`dpsolver` 是纯内核**：不含 `main`，不碰 stdin / stdout，可以直接当静态库链进别的宿主。
- **`net` 只依赖 `dpsolver`**，不需要 `cli`，也可以单独进 wasm（体积代价是那 6 MB 权重）。
- **`cli` 只调用公开 API**，是内核的一个使用者，也是唯一碰终端的地方。
- **`wasm` 是压平层**，把上面几层的接口摊成 `extern "C"` 的扁平函数。

仓库里**没有**工程文件（`.sln` / `.vcxproj`）、题目数据的 JSON / CSV 副本，也没有打包脚本——
这些都属于本机环境，换台机器重建即可。唯一的构建产物是 `bin/adventure.exe`
（静态链接，含 flash 权重，约 2 MB），方便不想装编译器的同学直接双击运行。

---

## 编译与运行

### Makefile（推荐，GCC / MinGW）

源码目录里带了一份 `Makefile`，目标文件和产物分别落在 `build/` 与 `bin/`：

```bash
mingw32-make              # 发布版 -> bin/adventure.exe
mingw32-make precal       # 打表版 -> bin/adventure_precal.exe（-DPRE_CAL）
mingw32-make check        # 只做 -fsyntax-only 语法检查，不产出任何文件
mingw32-make clean        # 删 build/
mingw32-make distclean    # 连 bin/ 一起删
```

release 版默认静态链接（`-static -static-libgcc`），拷走 `.exe` 即可直接运行，
不需要 MinGW 运行时 DLL。可覆盖的变量：

```bash
mingw32-make OPT=-O3 NATIVE=1     # 加 -march=native，仅本机用
mingw32-make STATIC=0             # 动态链接，体积更小
mingw32-make CC=clang             # 换编译器
```

> `make` 的 recipe 是 POSIX shell 写法，请在 Git Bash / MSYS2 里跑。
> `check` 只解析语法、不链接也不执行求解器，长时间算表时也可以放心跑。

### gcc / clang（手工一行）

```bash
gcc -std=gnu11 -O2 -fopenmp -Isrc/dpsolver -Isrc/net -Isrc/cli \
    src/dpsolver/*.c src/net/*.c src/cli/*.c src/main.c -o adventure
./adventure solve --role 3 --flash
```

`-Isrc/dpsolver -Isrc/net -Isrc/cli` 是必需的：跨模块头文件按逻辑名互相引用
（例如 `wasm_api.h` 里写的是 `"Adventure.h"`），靠 `-I` 才能解析。

`-fopenmp` 只有打表（`PRE_CAL`）时才真正需要，纯查表运行可以去掉。
`src/net/net.c` 会把 6 MB 的 `weights.h` 一起编进去，这一份翻译单元明显比别的慢。

只想编成**不含 main 的内核库**：不把 `cli/` 和 `main.c` 加进编译即可。

```bash
gcc -std=gnu11 -O2 -Isrc/dpsolver -Isrc/net -c \
    src/dpsolver/Adventure.c src/dpsolver/dpCal.c src/dpsolver/hash.c \
    src/net/net.c src/net/net_api.c
ar rcs libadventure.a Adventure.o dpCal.o hash.o net.o net_api.o
```

不需要 flash 的话，去掉 `src/net/*` 和 `src/cli/cli.c` 里的 `net_api.h` 引用即可得到一个
更小的纯 DP 库（`cli.c` 目前无条件依赖 `net_api.h`，只编库不受影响）。

### Windows / MSVC

新建一个空的控制台项目，把 `src/` 下的 `.c` 全部加进去（`wasm/` 除外），
在「附加包含目录」里填 `src/dpsolver;src/net;src/cli`，或者直接用命令行：

```bat
cl /O2 /openmp /Isrc\dpsolver /Isrc\net /Isrc\cli ^
   src\dpsolver\Adventure.c src\dpsolver\dpCal.c src\dpsolver\hash.c ^
   src\net\net.c src\net\net_api.c src\cli\cli.c src\main.c
```

> 代码要**同时在 MSVC、gcc、emcc 下编译**。某些 gcc 能接受的写法 VS 会直接报错，
> 例如 `#pragma omp for` 的循环变量**必须在 `for` 外声明**——改代码时按 MSVC 的限制来写。

### WebAssembly / Emscripten

```bash
emcc src/dpsolver/Adventure.c src/dpsolver/dpCal.c src/dpsolver/hash.c \
     src/net/net.c src/net/net_api.c src/wasm/wasm_api.c \
  -Isrc/dpsolver -Isrc/net \
  -O3 --no-entry -fcommon -DWASM \
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
之后按下面的导出接口调用。要省体积就把 `src/net/*` 去掉——flash 是可选件。

> 若要跑在**小程序**里，还需要把 Emscripten 胶水中的加载器换成 `WXWebAssembly`，
> 并把可选链、空值合并、逻辑赋值、class 字段等 ES2020+ 语法降级
> （部分真机 JS 引擎会直接 SyntaxError）。这部分适配脚本属于宿主环境，未包含在本仓库。

---

## 命令行用法

编译出的 `adventure` 有两种模式：**argv 子命令**，以及不带子命令时进入的**交互式 REPL**。
两种模式都只调用公开接口，不碰内核内部字段。

### argv 模式

```bash
./adventure solve --role 3 --flash
./adventure solve --role 3 --target 620 --precision
```

| 选项 | 含义 | 默认 |
|---|---|---|
| `--role N` | 角色编号 1~7 | 6（xiao_yu） |
| `--flash` | 神经网络推理（默认） | **开** |
| `--precision` | 精确 DP / 查表 | 关 |
| `--target T` | 目标总分；`620` / `480` 走多局冲刺模型，其余走单局模型 | 620 |
| `--round N` | 已完成局数 0~6 | 0 |
| `--score S` | 之前几局累计得分 | 0 |
| `--quiet` | 只打印总结 | 关 |

`solve` 会把本局一路打到结束，每一步打印它认为最优的题目与选项，
以及该步之后的达标概率或模型置信度：

```
Role  : 6 (xiao_yu)
Attrs : hp=3  pow=4  wis=5  cha=5  luc=4  agi=3
Model : flash (Neural Network, zero-search)
Note  : --target has no effect in flash mode
...
Round score : 72
Total score : 72  (prior 0 + this round 72)
Progress    : round 1 of 7 finished
Final attrs : hp=-2  pow=8   wis=5   cha=6   luck=3   agi=7
Success P   :  64.924% (model estimate at the first step)
```

> 每一步的题目是**从当前阶段还没选过的题里随机抽一道**，然后由引擎决定选哪个选项。
> 也就是说命令行只优化"选项"，不优化"选题"——真实游戏里题目也是随机给出的，
> 这个顺序和实际情况一致。想逐题对比就进 REPL 用 `q` / `opt Q` 手动看。

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
| `q` | 列出本阶段还能选的题：题目名、可行选项、引擎选择 |
| `opt Q` | 列出第 Q 题的四个选项：选项名、门槛、奖惩、达标概率 |
| `pick Q O` | 用第 O 个选项回答第 Q 题 |
| `auto` | 剩下的题全部自动最优推进 |
| `flash` / `precision` | 切换决策引擎 |
| `disparity` | 打印 flash 与 DP 的实测差距 |
| `reset` / `help` / `quit` | 重开 / 帮助 / 退出 |

`q` 和 `opt Q` 会带出题目、选项的英文名（来自 `guide_table.h`），
门槛和奖惩则**永远从内核题库现算**——攻略表里个别笔误不会污染显示。
门槛列里 `impossible` 表示该选项是 127 哨兵，无论属性多高都不会成功。

> 提示语全部是纯 ASCII，避免在非 Windows 终端（UTF-8）上出现乱码。

---

## 预计算打表（PRE_CAL）

概率表默认是 `dpCal.c` 的 `#else` 分支里那两张**已算好并固化的常量表**。
如果要重新生成（例如改了规则），走 `PRE_CAL` 流程。
打表是**离线一次性的重活**：状态空间大、递归深，8 线程跑完需要耐心。

1. 取消注释 `src/dpsolver/common.h` 里的 `#define PRE_CAL`（或编译加 `-DPRE_CAL`）
2. 编译，然后运行 `./adventure table --out .`
   （内部依次算 80 分档与 220 分档，并把结果写进 `winRate80.txt` / `winRate220.txt`）

   用 Makefile 的话就是：

   ```bash
   mingw32-make precal
   ./bin/adventure_precal.exe table --out .
   ```
3. 把两个文本的内容分别粘贴进 `src/dpsolver/dpCal.c` 的 `#else` 分支
   （`win_rates80` / `win_rates220` 两处静态常量表）
4. 关掉 `PRE_CAL`，重新编译——之后启动即查表，没有任何 DP 计算

> flash 用的权重与这两张表**没有关系**：网络是另外训练的，改规则后需要重新训练并导出一份
> 新的 `weights.h`，只重算概率表不会让 flash 变准。

---

## C API 速览

`src/dpsolver/Adventure.h` 暴露的接口大致分四组（完整声明见头文件）：

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
| 精确 DP | `game_evaluate_options` | 评估某题 **4 个选项**各自的期望分与达标概率 |
| | `game_evaluate_options_sprint` | 冲刺模式下的选项评估（指定 620 / 480 目标） |
| | `game_query_expect` | 当前局的剩余期望最高分 |
| | `game_query_goal_prob` | 当前局达到目标 T 的概率 |

flash 侧的对应入口在 `src/net/net_api.h`：

```c
/* 202 维特征由内核自己构造，调用方不用管特征工程 */
int flash_evaluate_options(GameContext *ctx, int q_idx, int target_T, OptionEval out[4]);

/* 只要动作/价值、手上有现成特征时用这个（输入长度必须正好 202） */
int flash_predict(const float *flat_input, float *p_out, float *v_out);
```

`flash_evaluate_options` 的语义与 `game_evaluate_options` 不同：**返回值就是模型选出的
最优选项下标 0..3**（`<0` 表示推理失败），`out[i].prob_goal` 是四个选项各自的
softmax 概率，`out[i].expect_remain` 是同一份状态价值（四项相同）。

典型调用顺序：

```c
GameContext *ctx = game_ctx_create(ROLE_XIAO_YU);

int total = game_ctx_type_total(ctx);
for (int q = 0; q < total; q++) {
    if (!game_ctx_type_available(ctx, q)) continue;

    OptionEval ev[4];

    /* 快：一次前向，返回值就是 pick */
    int pick_nn = flash_evaluate_options(ctx, q, /*target_T=*/70, ev);

    /* 或精确：ev[i].expect_remain / prob_goal */
    game_evaluate_options(ctx, q, /*target_T=*/82, ev);

    StepResult sr;
    game_do_step(ctx, q, pick_nn, &sr);
}

game_ctx_destroy(ctx);
```

---

## WebAssembly 导出接口

`src/wasm/wasm_api.c` 把上面的 C API 压平成 wasm 友好的一层（全部 `extern "C"`）：

- 基础：`wasm_game_create` / `wasm_game_destroy` / `wasm_game_do_step` / `wasm_game_evaluate`
- flash：`wasm_game_evaluate_flash`（语义同 `flash_evaluate_options`，返回最优选项下标）
- 快照续档：`wasm_game_create_form` / `wasm_game_ctx_modify` /
  `wasm_game_snapshot_fill` / `wasm_game_snapshot_size`
  （`GameSnapshot` 的内存布局在 `Adventure.h` 里逐字节写明，JS 侧不用手算偏移）
- 冲刺：`wasm_sprint_winrate` / `wasm_sprint_evaluate`
- 版本：`wasm_abi_version()`（当前返回 `GAME_VERSION = 3`），前端启动时校验，
  避免 wasm 与页面版本错配

JS 侧用 `_malloc` / `_free` 分配 `OptionEval[4]`（64 字节）等结构体，
通过 `HEAPF64` 或 `Module.getValue(ptr, "double")` 读结果。

> ⚠️ `wasm_sprint_winrate` **目前仍是占位实现**，固定返回 `out[0] = out[1] = 0.0`。
> 冲刺胜率请走 `wasm_sprint_evaluate`（它已经接到真实的 DP），
> 或者由宿主自己按 `able_lost_s` 的换算口径查表。

---

## 已知限制与注意事项

- **flash 不是最优解**。它是策略网络的预测，只在统计意义上接近 DP；
  需要确定性答案（对账、跑分、出报告）时请用 `--precision`。
- **特征口径必须与训练侧一致**。`src/net/net_api.c` 是 C 侧的唯一权威，
  对应 Python 的 `features.py` / `game.py` / `norm.py`。任何一处归一化规则改动
  都不会报错，只会让推理结果**静默跑偏**——改这里时把 Python 侧的 `to_vec()` 一起对照。
- **`ops.h` 里的算子不是 `static`**，它只能被一个翻译单元包含（当前只有 `net.c`）。
  想多处复用就先改成 `static inline`，否则会撞重复定义。
- **权重以 C 源码形式存在**。`weights.h` 约 6.3 MB / 5.8 万行，编译 `net.c` 明显比其他
  文件慢，增量构建时也躲不开它。改权重只能重新导出整份头文件。
- **编译器以 MSVC 为准**。代码要同时在 MSVC、gcc、emcc 下编译；`#pragma omp for` 的
  循环变量必须在 `for` 外声明，否则 VS 直接报错。
- **key 的位宽是硬约束**。`able_lost_s` 在 key 里只占 8 bit（最大 255），
  当前的 80 / 220 两档都在范围内；若将来放宽丢分上限，必须同步扩位，否则会 key 撞车。
- **默认是原生控制台构建**。编 WebAssembly 时必须显式加 `-DWASM`，
  否则会去编 `main.c` / `cli.c`；反过来只想出库文件时，把这两个文件排除即可。
- **源码编码为 UTF-8（带 BOM），换行统一 CRLF**，整个 `src/` 树一致，含中文注释。
  VS 可直接识别；gcc / emcc 默认按 UTF-8 读，不需要额外 `-finput-charset`。
  新增文件请保持同样的编码，否则中文注释会乱码，且 MSVC 在 GBK 代码页下可能误判。
- **题库数据内嵌**。改题目要改 `src/dpsolver/Adventure.c` 里的 `static const` 表，
  规则常量（`totalQs` / `needPick` / `successScore` / `failScore`）在同文件，注意保持一致；
  `guide_table.h` 里的题名和 `net_api.c` 里的 `k_arch_scale`（各类主属性题目数）
  也要跟着改，否则显示错位、特征失真。
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
