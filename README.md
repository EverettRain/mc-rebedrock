# MC ReBedrock

**ReBedrock** 是一个用 **C++20 + Vulkan** 从零编写的体素沙盒引擎——包含图形客户端、
权威游戏运行时，以及一个实验性的无头专用服务器。它以 **Minecraft Java Edition 26.1**
的资源格式、数据结构与玩法行为为**参照**，但渲染、模拟、存档全部为独立实现，并采用
**Bedrock 风格的数据驱动模型/动画**（geo.json + animation.json + Molang）来呈现内容。

> ReBedrock 是非官方项目，与 Mojang Studios 或 Microsoft 无关。仓库与发行包**不包含**
> Mojang 的纹理、音效、字体、翻译或任何受版权保护的游戏资源——运行时需你自备合法资源包。

目前处于 **Beta** 阶段，主要目标平台为 **macOS ARM64（MoltenVK）** 与 **Windows x64
（原生 Vulkan）**。它不是 Minecraft 的完整复刻，也不能直接读取 Java 版世界存档。

[简体中文更新日志](CHANGELOG_CH.md) · [English Changelog](CHANGELOG_EN.md)

---

## 设计原则

ReBedrock 不是"照抄一遍"，而是围绕几条工程原则重写：

- **确定性优先** —— 世界生成、红石、实体、经验散射、随机 tick 全部走可复现的
  Java 风格 RNG，**不依赖墙钟或全局随机**，同输入同输出，可回放、可无头验证。
- **数据导向（DOD）** —— 方块/物品/实体是稠密整数 id，行为是数据（函数指针 / `std::function` /
  constexpr 表），而非对象图与 `switch`；状态用混合基数打包，实体用 SoA 池。
- **双端架构** —— 渲染端与权威 20 TPS 运行时通过统一二进制消息流通信；单人是进程内
  loopback，同一 `MessageChannel` 可接到带版本握手的 TCP。**专用服务器不链接 Vulkan/GLFW**，
  可在无图形环境开服。
- **自备资源、零版权负担** —— 引擎不内置任何 Mojang 资源，改用标准 Java 资源包在运行时加载。

---

## 功能一览

### 世界与渲染
确定性种子地形、生物群系、区块流送、调色板压缩的方块状态、增量方块光与天空光、平滑光照、
昼夜与月相、天气、GPU 粒子、雨雪，以及实验性太阳阴影。

### 多维度
**主世界 / 下界 / 末地**三维度，各自的地形生成与群系；**逐维度独立 tick**（未加载的维度
零开销），跨维度查询在无区块加载时直接跳过；存档按维度分区目录存储（镜像 vanilla 的
`region` / `DIM-1` / `DIM1` 布局）。

### 玩法
20 TPS 权威模拟、玩家物理与体素碰撞、生存/创造、生命/饥饿/氧气、挖掘与放置、物品掉落、
农作物、流体、重力方块、箱子与独立熔炉方块实体、2×2 / 3×3 合成，以及台阶/栅栏等形状方块
（单一形状源，碰撞/射线/剔除一致）。

### 经验系统
完整的等级/进度/总量状态与 26.1 升级曲线、HUD 经验条与等级数字、**经验球实体**（物理 +
同值合并 + 磁力吸附 + 拾取），以及**击杀 / 采矿 / 熔炼 / 繁育**四类来源与死亡掉落
（`min(7·等级, 100)`、`keepInventory` 联动）——为后续附魔/铁砧预留稳定的消耗接缝。

### 实体
猪、牛、僵尸、尸壳，含 box-UV 骨骼动画、自然生成、寻路、受伤与战斗、物种音效、空间索引、
模拟距离与存档持久化。

### 动画（Bedrock schema）
自研的数据驱动动画库：`SkeletalModel` + `AnimationClip`（含 bezier 插值）+ **Molang 表达式
求值**（`query.*` / `variable.*` / `math.*` / 三元 / 序列，编译一次求值多次）+ 加权分层混合。

### 音频
11 类声音分类与各自音量（对标 vanilla `SoundSource`）、距离衰减与 3D 立体声、Directional
Audio 开关、biome 环境声与洞穴 mood、情境音乐与唱片机。

### 命令
Brigadier 风格命令树、Tab 补全（含值类型上下文补全）、`/help` 自省与智能用法生成、
**op 权限阶梯 + Allow Cheats 世界开关**，以及 **`/execute` 真 redirect 子命令树**。已实现：
`gamemode`、`time`、`give`、`gamerule`、`tp`、`kill`、`spawnpoint`、`weather`、`setblock`、
`fill`、`summon`、`difficulty`、`seed`、`clear`、`experience`(`/xp`)、`execute`、`help`。

### 界面
原版风格 HUD、背包与容器、创造物品栏、标题全景、暂停与选项页面；资源包驱动的语言/字体/
26.1 GUI 精灵缩放元数据。界面布局与像素几何以内部整理的 26.1 GUI 规格为权威参照。

### 运行架构与存档
统一二进制消息流交换移动/玩法命令、事件与玩家/世界/实体快照；渲染器只读客户端镜像与区块
缓存。自定义多世界二进制存档格式，保存方块状态、方块实体、实体、掉落物、经验球、时间、
天气、游戏规则与玩家状态，带**自描述版本头**、逐维度分区与迁移/校验。该格式**不兼容** Java NBT。

更细的已实现内容与行为修复见[简体中文更新日志](CHANGELOG_CH.md)。

---

## 运行时资源包

ReBedrock **必须**在运行时加载至少一个标准 Java 资源包。首次启动会在游戏目录创建
`resourcepacks/`；若其中没有可用资源包，程序会打印完整路径并退出。

把包含 `pack.mcmeta` 与 `assets/` 的资源包目录或 `.zip` 放入：

```text
game/
├── bin/
├── config/
├── resourcepacks/
│   ├── vanilla-26.1.zip
│   └── my-pack/
│       ├── pack.mcmeta
│       └── assets/
└── saves/
```

资源包根目录必须直接包含 `pack.mcmeta`，不能在 zip 内再套一层目录。项目按 Java 26.1 资源包
格式处理资源，支持：

- `assets/<命名空间>/` 下的纹理、GUI 精灵、语言、字体与 `sounds.json`
- `bitmap` / `space` / `reference` / `unihex` 字体提供器
- 纹理动画 `.png.mcmeta` 与 GUI `nine_slice` / `tile` / `stretch` 元数据
- 目录形式资源包的 `overlays.entries`
- `data/<命名空间>/tags/blocks/` 方块标签；缺失时使用内置的 26.1 默认标签

多个资源包按文件名排序，排在最后者优先级最高。压缩包资源直接从 zip 读取；`.packcache/`
仅用于必要的兼容缓存，不应加入版本控制。你必须从自己合法拥有的 Minecraft 安装中准备资源。

---

## 构建

### 要求

- CMake 3.25+
- 支持 C++20 的编译器
- Vulkan SDK（Vulkan 1.2 头文件、加载器与 `glslc`）
- 支持 Vulkan 1.2 与交换链的 GPU/驱动
- Git + 网络（首次拉取 GLFW 3.4、GLM、Vulkan Memory Allocator）
- macOS 上如需交叉编译 Windows x64：`brew install mingw-w64`

音频/图片/zip 分别使用 miniaudio + stb_vorbis、stb_image 与 vendored miniz。

### 构建与测试

项目维护 `build/rebedrock-debug` 与 `build/rebedrock-release` 两个规范构建目录。在装有
Vulkan SDK 与 MinGW-w64 的 macOS 上，一条命令即可构建两配置、交叉编译 Windows x64，并组装
不含 Mojang 资源的双平台发行包：

```sh
./build.sh --test
```

只构建当前平台时直接用 CMake（macOS 上关闭默认的 Windows 伴随构建）：

```sh
cmake -S . -B build/rebedrock-debug -DBUILD_TESTING=ON \
  -DMC_REBEDROCK_BUILD_WINDOWS_ON_MAC=OFF
cmake --build build/rebedrock-debug --parallel 8
ctest --test-dir build/rebedrock-debug --output-on-failure
```

模拟链打成静态库 `mc_rebedrock_runtime`，游戏、无头服务端与绝大多数测试都链接它，一次改动
只编一遍。benchmark 与存档迁移诊断等手动工具默认不构建，需要时加 `-DMC_REBEDROCK_BUILD_TOOLS=ON`。

构建产物：

```text
build/rebedrock-debug/game/bin/mc_rebedrock
build/rebedrock-release/game/bin/mc_rebedrock
build/rebedrock-release/game/bin/mc_rebedrock.exe          # macOS 交叉编译时
build/rebedrock-debug/tests/mc_rebedrock_dedicated_server  # BUILD_TESTING=ON, POSIX
```

`./build.sh` 还会生成 `build/mc-rebedrock-<版本>-win-mac.zip`；发行包只含可执行文件、编译后
的着色器与 ReBedrock 自有资源，使用者仍需自行把标准资源包放入 `resourcepacks/`。

---

## 运行

把资源包放入对应构建目录的 `game/resourcepacks/`，然后：

```sh
./build/rebedrock-debug/game/bin/mc_rebedrock          # Windows 用 mc_rebedrock.exe
```

程序在同一 `game/` 根目录维护 `config/options.properties`、`config/rebedrock.log`、
`resourcepacks/`、`saves/` 与运行时 `.packcache/`。

**无头专用服务器**（在启用 `BUILD_TESTING` 的 POSIX 开发构建中生成）可本地运行或等待 TCP 客户端：

```sh
./build/rebedrock-debug/tests/mc_rebedrock_dedicated_server \
  --save-dir ./server-saves --world dedicated --view-distance 8 --port 25565
```

省略 `--port` 只在本地无头运行；`--ticks N` 用于有限 Tick 冒烟测试。当前图形客户端尚未提供
远程地址入口，网络路径主要由 headless 端到端测试覆盖。

渲染回归的隔离场景参数：

```sh
mc_rebedrock --test-scene minecraft:stone --stage 0 [--occlusion-scene]
```

---

## 项目结构

```text
src/assets/       标准资源包、字体、声音、图片与元数据
src/audio/        音频设备、分类音量、空间化与环境/音乐
src/animation/    Bedrock schema 骨骼模型、动画剪辑与 Molang 求值
src/client/       客户端玩家/世界/实体镜像
src/gameplay/     会话、物品、方块实体、实体、经验与模拟规则
src/net/          消息协议、loopback、TCP 与连接握手
src/persistence/  自定义多世界存档与迁移
src/render/       相机、粒子、测试场景与 Vulkan 渲染
src/runtime/      权威世界生命周期、Tick、流送与持久化协调
src/server/       无头主机、专用服务器与命令行入口
src/ui/           HUD、菜单、字体与本地化
src/world/        区块、方块状态、光照、维度、流送与地形生成
resources/        项目自有着色器、动画、语言与默认配置
tests/            单元测试、回归测试与基准探针
```

---

## 已知边界

诚实地说，ReBedrock 还是一个进行中的项目：

- **内容规模有限**：只覆盖已注册的一部分方块、物品、生物与配方。
- **资源解析**面向 ReBedrock 当前使用的 Java 26.1 资源子集，并非通用 Minecraft 客户端或完整
  数据包实现（`.mcfunction`、完整数据包/资源包管理仍在推进）。
- **网络多人**：已有单连接 TCP 专用服务器骨架与协议握手，但图形客户端远程入口、多连接玩家
  所有权、断线重连与完整联机尚未实现。
- **附魔、完整红石内容、Java 版世界导入**尚未实现（确定性红石信号评估器已具备，元件内容在推进）。
- 实验性雨、粒子与太阳阴影路径可能随开发继续调整。

---

## 版权与资源

ReBedrock 的源代码为本项目原创实现。仓库与发行包**不含**任何 Mojang 版权资源；运行 ReBedrock
需要你从自己合法拥有的 Minecraft 安装准备标准 Java 资源包。Minecraft 是 Mojang Studios 的商标。
