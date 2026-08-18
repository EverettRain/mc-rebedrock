# MC ReBedrock

ReBedrock 是一个使用 C++20 和 Vulkan 从头开发的体素沙盒实现，包含图形客户端、
权威游戏运行时和实验性无头专用服务器。项目以
Minecraft Java 26.1 的资源格式、数据结构和玩法行为为参照，但使用独立的渲染、
模拟与存档实现。

目前项目处于 Beta 阶段，主要目标平台为 macOS ARM64（MoltenVK）和 Windows x64
（原生 Vulkan）。它不是 Minecraft 的完整复刻，也不能直接读取 Java 版世界存档。

> ReBedrock 是非官方项目，与 Mojang Studios 或 Microsoft 无关。仓库和发行包均不
> 包含 Mojang 的纹理、音效、字体、翻译或其他受版权保护的游戏资源。

[更新日志](CHANGELOG_CH.md) · [Changelog](CHANGELOG_EN.md)

## 当前功能

- **世界与渲染**：确定性种子地形、生物群系、区块流送、调色板压缩方块状态、增量
  方块光与天空光、平滑光照、昼夜和月相、天气、GPU 粒子、雨雪以及实验性太阳阴影。
- **玩法**：20 TPS 模拟、玩家物理和体素碰撞、生存/创造模式、生命/饥饿/氧气、
  挖掘与放置、物品掉落、农作物、流体、重力方块、箱子、独立熔炉方块实体，以及
  2×2/3×3 合成。
- **实体**：猪、牛和僵尸，包含 box-UV 骨骼动画、自然生成、寻路、受伤与战斗、
  物种音效、空间索引、模拟距离和存档持久化。
- **界面**：原版风格 HUD、背包与容器、创造物品栏、标题全景、暂停和选项页面，
  支持资源包驱动的语言、字体和 26.1 GUI 精灵缩放元数据。
- **命令**：Brigadier 风格命令树及 Tab 补全，已实现 `/gamemode`、`/time`、
  `/give`、`/gamerule`、`/tp`、`/kill`、`/spawnpoint` 和 `/weather`。
- **运行架构**：渲染端与权威 20 TPS 运行时通过统一二进制消息流交换移动/玩法命令、
  事件及玩家/世界/实体快照；单人游戏使用进程内 loopback，实验性专用服务器可把同一
  `MessageChannel` 接到带版本握手的 TCP socket。渲染器只读取客户端镜像和区块缓存。
- **存档**：自定义多世界二进制格式，保存方块状态、方块实体、实体、掉落物、时间、
  天气、游戏规则和玩家状态，并包含版本迁移与校验。该格式不兼容 Java 版 NBT。

更细的已实现内容和行为修复请查看[简体中文更新日志](CHANGELOG_CH.md)。

## 运行时资源包

ReBedrock **必须**在运行时加载至少一个标准 Java 资源包。首次启动会在游戏目录创建
`resourcepacks/`；如果目录中没有可用资源包，程序会打印其完整路径并退出。

将包含 `pack.mcmeta` 和 `assets/` 的资源包目录或 `.zip` 放入：

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

资源包根目录必须直接包含 `pack.mcmeta`，不能在 zip 内再套一层目录。项目当前按
Java 26.1 的资源包格式 84 处理资源，并支持：

- `assets/<命名空间>/` 下的纹理、GUI 精灵、语言、字体和 `sounds.json`
- `bitmap`、`space`、`reference` 和 `unihex` 字体提供器
- 纹理动画 `.png.mcmeta` 与 GUI `nine_slice` / `tile` / `stretch` 元数据
- 目录形式资源包的 `overlays.entries`
- `data/<命名空间>/tags/blocks/` 方块标签；缺失时使用内置的 26.1 默认标签

多个资源包按文件名排序，排在最后的资源包优先级最高。压缩包资源直接从 zip 读取；
`.packcache/` 只用于必要的兼容缓存，不应加入版本控制。

你必须从自己合法拥有的 Minecraft 安装中准备所需资源。旧的
`resources/vanilla/1.16.1/` 提取目录已经不再参与当前运行或打包流程。

## 构建要求

- CMake 3.25 或更高版本
- 支持 C++20 的编译器
- Vulkan SDK，包含 Vulkan 1.2 头文件、加载器和 `glslc`
- 支持 Vulkan 1.2 与交换链功能的 GPU/驱动
- Git 和网络连接，用于首次获取 GLFW 3.4、GLM 与 Vulkan Memory Allocator
- macOS 上如需同时交叉编译 Windows x64：`brew install mingw-w64`

音频、图片和 zip 支持分别使用 miniaudio/stb_vorbis、stb_image 与 vendored miniz。

## 构建与测试

项目维护两个规范构建目录：`build/rebedrock-debug` 和
`build/rebedrock-release`。在已安装 Vulkan SDK 和 MinGW-w64 的 macOS 上，可用一条
命令构建两个配置、交叉编译 Windows x64，并组装不含 Mojang 资源的双平台发行包：

```sh
./build.sh --test
```

常用参数：

```sh
./build.sh --configure   # 重新运行 CMake 配置
./build.sh -j4           # 使用 4 个并行构建任务
./build.sh --help
```

只构建当前平台时，可直接使用 CMake。macOS 上关闭默认启用的 Windows 伴随构建：

```sh
cmake -S . -B build/rebedrock-debug \
  -DBUILD_TESTING=ON \
  -DMC_REBEDROCK_BUILD_WINDOWS_ON_MAC=OFF
cmake --build build/rebedrock-debug --parallel 8
ctest --test-dir build/rebedrock-debug --output-on-failure
```

构建结果位于：

```text
build/rebedrock-debug/game/bin/mc_rebedrock
build/rebedrock-release/game/bin/mc_rebedrock
build/rebedrock-release/game/bin/mc_rebedrock.exe   # macOS 交叉编译时
build/rebedrock-debug/tests/mc_rebedrock_dedicated_server  # BUILD_TESTING=ON，POSIX
```

`./build.sh` 还会生成 `build/mc-rebedrock-<版本>-win-mac.zip`。发行包只包含可执行
文件、编译后的着色器和 ReBedrock 自有资源；使用者仍需自行把标准资源包放入包内的
`resourcepacks/`。

## 启动

先把资源包放入对应构建目录的 `game/resourcepacks/`，再运行：

```sh
./build/rebedrock-debug/game/bin/mc_rebedrock
```

Windows 使用 `mc_rebedrock.exe`。程序会在同一个 `game/` 根目录维护：

- `config/options.properties`：图形、控制、语言和实验性选项
- `config/rebedrock.log`：追加写入的运行日志
- `resourcepacks/`：用户提供的标准资源包
- `saves/`：ReBedrock 自定义世界存档
- `.packcache/`：运行时资源缓存

用于渲染回归的隔离场景参数：

```sh
mc_rebedrock --test-scene minecraft:stone --stage 0
mc_rebedrock --test-scene minecraft:stone --stage 0 --occlusion-scene
```

`--stage` 接受 `0..9`，表示方块破坏阶段；`--test-scene` 也接受注册表中的数字方块
ID。开发与自动化还提供若干 `MC_REBEDROCK_*` 环境变量，具体行为以相关源码注释为准。

实验性无头服务器在启用 `BUILD_TESTING` 的 POSIX 开发构建中生成，可本地运行或等待
一个 TCP 客户端：

```sh
./build/rebedrock-debug/tests/mc_rebedrock_dedicated_server \
  --save-dir ./server-saves --world dedicated --view-distance 8 --port 25565
```

省略 `--port` 时只在本地无头运行；`--ticks N` 可用于有限 Tick 的冒烟测试。当前图形
客户端尚未提供远程地址入口，因此网络路径主要由 headless 端到端测试验证。

## 项目结构

```text
src/assets/       标准资源包、字体、声音、图片和元数据
src/audio/        音频设备与播放
src/client/       客户端玩家/世界/实体镜像
src/gameplay/     会话、物品、方块实体、实体和模拟规则
src/net/          消息协议、loopback、TCP 与连接握手
src/persistence/  自定义存档与迁移
src/render/       相机、粒子、测试场景和 Vulkan 渲染
src/runtime/      权威世界生命周期、Tick、流送与持久化协调
src/server/       无头主机、专用服务器及命令行入口
src/ui/           HUD、菜单、字体和本地化
src/world/        区块、方块状态、光照、流送与地形生成
resources/        项目自有着色器、动画、语言和默认配置
tests/            单元测试、回归测试与基准探针
```

## 已知边界

- 当前内容规模只覆盖项目已注册的一部分方块、物品、生物和配方。
- 资源解析目标是满足 ReBedrock 当前使用的 Java 26.1 资源子集，并非通用 Minecraft
  客户端或完整数据包实现。
- 已有单连接 TCP 专用服务器骨架与协议握手，但图形客户端远程连接入口、多连接玩家所有权、
  断线重连和完整网络多人游戏尚未实现。
- Java 版世界导入和完整红石系统尚未实现。
- 实验性雨、粒子和太阳阴影路径可能随开发继续调整。
