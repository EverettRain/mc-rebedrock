# 完整帧间隔与跨线程性能采集

这套采集用于解释两类问题：提高 `random_tick_speed` 后画面和世界推进卡顿；进入地图或移动
增删区块时掉帧。它只增加诊断，不调整随机刻次数、模拟范围、共享锁、线程数或流送预算。

## 三种数据各自回答什么

| 数据 | 能回答 | 不能单独回答 |
|---|---|---|
| PerfTrace 的 CPU 时间线 | 相邻帧起点之间的墙钟组成、显式锁等待、tick 与流送阶段重叠、队列变化 | 某段墙钟内线程究竟运行了多久、被系统换出多久、任意函数的采样栈 |
| 可选 GPU 时间戳 | 对应生产帧的 GPU 帧图跨度与阶段边界耗时 | 显示器实际扫描输出时间、合成器丢帧、CPU/GPU 未校准时钟的绝对对齐 |
| Instruments 的 Time Profiler / System Trace / Metal System Trace | 热点调用栈、线程调度、驱动与 GPU/呈现行为 | 引擎中的随机刻尝试次数和区块语义，仍需时间线计数器 |

“完整”指建立完整帧间隔的边界，并显式保留尚未细分的余量，不代表靠计时 scope 就能知道
任意指令的成本。CPU scope 是墙钟，包含运行、等待和被调度出去的时间。

## 与旧 FrameTrace 的关系

旧 `MC_REBEDROCK_FRAME_TRACE` 保留原有超阈值 stdout 日志。它的 `cpuMs` 在帧尾日志、
内存报告、限速 sleep/spin 之前结束；`unaccMs≈0` 只证明那个较短区间的分段闭合。
新采集独立开关，默认不启用旧日志或额外 GPU 探针。

只用超过 16.67ms 的旧日志，会漏掉从 120 FPS 降到 90–100 FPS 的大量帧；只对这些日志做
分位数，则得到的是被筛选的慢帧分布。新采集应使用窗口内的全部完整帧，最后未闭合帧及
采集窗口边界的不完整帧不作为常规分位数样本。

## 采集与分析

| 环境变量 | 含义 |
|---|---|
| `MC_REBEDROCK_PERF_TRACE` | 输出 JSON 路径；未设置或为空时关闭 |
| `MC_REBEDROCK_PERF_TRACE_MAX_EVENTS` | 默认 200000，硬上限 1000000 个事件；超过容量会明确标记丢失 |
| `MC_REBEDROCK_PERF_TRACE_DELAY_SECONDS` | 从采集器初始化起延迟多少秒开始记录 |
| `MC_REBEDROCK_PERF_TRACE_DURATION_SECONDS` | 记录窗口长度；未设置或为 0 时不限制时间，但仍限制事件容量 |
| `MC_REBEDROCK_PERF_TRACE_GPU` | 与 PerfTrace 一起设置时额外开启 GPU 查询；默认关闭，需单独测量开销 |

使用优化构建。当前 macOS 的 `build/rebedrock-release` 是 `RelWithDebInfo`，仍有 `-O2`
优化与调试符号，适合采样栈。不要把另一个平台或另一个二进制的结果当成本次构建结果。

从实际发行目录启动游戏，设置 `MC_REBEDROCK_PERF_TRACE` 为一个新的 JSON 文件路径。
该目录必须已存在且可写。正常退出后才读取结果；采集过程中不逐帧写盘。

```bash
cd build/rebedrock-release/game
MC_REBEDROCK_PERF_TRACE=/tmp/rebedrock-stationary.json ./bin/mc_rebedrock
```

需要先进入地图、设置参数时，例如预留 60 秒准备，再记录 30 秒：

```bash
MC_REBEDROCK_PERF_TRACE=/tmp/rebedrock-random-1000.json \
MC_REBEDROCK_PERF_TRACE_DELAY_SECONDS=60 \
MC_REBEDROCK_PERF_TRACE_DURATION_SECONDS=30 \
MC_REBEDROCK_PERF_TRACE_MAX_EVENTS=1000000 ./bin/mc_rebedrock
```

窗口从采集器首次初始化计时，可能早于画面可交互；根据本机启动时间调整延迟。窗口结束后
仍需正常退出以导出。完全落在窗口之外或跨越边界的 span 会被过滤，不计作容量溢出；
边界上的 frame 不参与完整帧统计。

不同构建的可执行文件名或发行目录可能不同，请以本机文件为准。先做一次 5–10 秒短采集，
确认正常退出后输出有效 JSON、没有丢事件，再采集正式场景。

从项目根目录分析结果：

```bash
python3 tools/analyze_perf_trace.py /tmp/rebedrock-stationary.json
```

JSON 使用 Chrome Trace Event 格式，可在兼容的时间线查看器中打开。查看 render、simulation、
streamer 与 persistence 等线程的相同时段；不要把各线程的 scope 时长相加成“总帧成本”。
父 scope 已包含子 scope，也不能把嵌套项相加。

当前事件布局约 64 字节，默认预分配约 12.2 MiB，容量上限约 61 MiB；退出导出的快照
会暂时再占一份近似大小的内存。各线程通过原子计数取得独占槽位，记录时不争抢全局
互斥锁、不分配内存、不写磁盘；容量溢出会写入元数据。
出现丢事件时，该文件可以提示候选热点，
但不足以证明某阶段耗时为零或完成精确闭合；缩短窗口或在内存允许下提高容量再采集。
强制终止、崩溃、关机可能丢失尚未写出的捕获。

## 必须检查的数据

- 完整帧间隔和帧工作时间的 p50/p95/p99，帧限速 sleep/spin，以及未细分余量。
- 输入和事件消费、区块结果应用、丢弃物品逻辑的锁等待/持有。
- drawFrame 的 fence、acquire、image wait、上传准备、record、submit、present。
- 模拟 tick 的外层耗时、世界写锁等待/持有、gameplay/randomTicks、命令处理和快照发布。
- tick 调度延迟与 backlog reset，结合 tick 时间戳判断实际推进节奏；50ms 是 20 TPS 的预算。
- 随机刻候选/加载区块、非空 section、抽样次数、命中和状态变化。不要把抽样次数当成功更新数。
- 区块工作线程的生成、光照、网格、编辑批次、结果交付；队列深度和前台接收量。
- 后台持久化的等待、批次与保存耗时。

GPU 查询读回通常来自之前占用相同 frame slot 的提交。必须通过生产帧 ID 关联，不能把它
当成读回时这帧的 GPU 耗时。最后几帧可能在退出前还没回收查询，不应补零。

`frame.wall` 是相邻循环起点之间的完整墙钟；`frame.work` 是该帧起点到限速器之前，
不包含限速之后的烟测脚本或测试存档钩子，这些由单独的尾部 scope 覆盖。摘要中的独占
归因只在同一渲染线程、完整 frame.wall 内计算。未细分的父阶段仍然会显示为父阶段名称，
不能把很小的 residual 解读为每个函数都已被单独定位。

`random.sampled_sections` 是 ReBedrock 实际进入抽样的非空 section 数，**不是**维护了
随机刻方块计数的 section 数；S=0 时不枚举这部分 section，计数为零。该指标用于核对
`random.attempts = S × random.sampled_sections`，不能直接当作 vanilla 的资格统计。
调度器 sequence、runtime tick ID 与 server tick 属于不同标识域；跨域用同线程时序和嵌套
关系关联，不能假定它们在切换世界、暂停、同步 tick 模式下始终数值相等。

## 可复现的对照

先固定机器、电源模式、分辨率、画质、资源包、视距/模拟距离、帧限/VSync、天气和镜头。
每次记录 commit、工作区差异、可执行文件路径/哈希及 CMake 配置。相同 seed 不保证两个
引擎生成相同内容，应额外记录实际加载区块和 section 的数量。

| 场景 | 控制变量 | 目的 |
|---|---|---|
| 已加载完、静止、无随机刻方块的非空地形 | S=0/3/100/300/1000 | 分离无效抽样与锁等待 |
| 固定植物密度的静止场景 | 同上 | 分离 handler、事件和网格更新成本 |
| 已生成区域固定路线 | S=0 或 3，固定移动速度 | 分离加载/卸载/提交/上传 |
| 未生成区域固定路线 | 同上 | 分离新地形生成增量 |
| 首次进入同一地图 | 冷缓存和暖缓存分别统计 | 观察集中提交与分配峰值 |

当前代码将 `random_tick_speed=3000` 截为 1000；采集时确认实际规则回报。vanilla 可额外
测 3000，但不要标记为与当前 ReBedrock 等参数。模拟距离 12 的候选范围也有实现差异。

120 FPS 对应 8.33ms，100 FPS 对应 10ms，90 FPS 对应 11.11ms。先保持 120 帧限制复现
用户体验，再单独测不限帧来判断原先被帧率上限掩盖的余量。不要将前后的平均 FPS 差直接
当作某个 scope 的耗时变化。

## 外部采样：补齐线程调度与调用栈

本机 Xcode 的 `xctrace list templates` 已确认存在 Time Profiler、System Trace 和
Metal System Trace。以下命令在终端执行，`<PID>` 替换为此次游戏进程的 ID；每次选择一种
模板，输出路径不要复用已有文件：

```bash
xcrun xctrace record --template 'Time Profiler' --attach <PID> \
  --time-limit 30s --output /tmp/rebedrock-cpu.trace

xcrun xctrace record --template 'System Trace' --attach <PID> \
  --time-limit 30s --output /tmp/rebedrock-scheduling.trace

xcrun xctrace record --template 'Metal System Trace' --attach <PID> \
  --time-limit 30s --output /tmp/rebedrock-gpu.trace
```

这些工具可能需要系统的开发者工具权限。引擎时间线先确定慢帧落在哪段；CPU 采样再定位
该段的函数，System Trace 区分运行和调度等待，Metal System Trace 检查驱动、GPU 与呈现。
若不做时钟校准，不要将 CPU 的 steady-clock 和 GPU 原始 timestamp 强行画成同一绝对轴。

## 验证采集本身

使用同一短路线至少比较三组：不开诊断、只开 CPU PerfTrace、再启用 GPU 时间戳。
记录事件丢失和内存占用；GPU 时间戳本身在部分设备上会影响执行边界，因此不能默认其
测量开销为零。不要同时开启旧逐帧日志、额外内存报告和多个重型 profiler 后直接归因游戏。

有限窗口内没有锁等待，只能说明该窗口没观测到；很小的 fence 等待不能排除 GPU 受限；
没有完成的 tick 不能当作“tick 耗时为零”。最终优化验收同时看帧时间尾部、TPS/命令延迟、
队列是否积压以及可见区块补齐时间。
