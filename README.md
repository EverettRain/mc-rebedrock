# MC Rebedrock

基于 C++20 与 Vulkan 的自研体素沙盒项目，玩法与渲染对齐 Minecraft Java 26.1 的数据与行为。
macOS ARM64 通过 MoltenVK 映射到 Metal，Windows x64 使用原生 Vulkan 驱动。

## 特性

- **世界**：确定性种子地形生成、24000 Tick 昼夜循环、增量光照传播、水体流动与沙子下落模拟
- **玩法**：20 TPS 玩家物理与体素碰撞、生存/创造双模式、生命/饥饿/氧气、死亡与重生
- **交互**：挖掘/放置、背包与快捷栏、2×2 与 3×3 合成、熔炉烧炼、箱子、掉落物
- **生物**：猪 / 牛 / 僵尸，box-UV 蒙皮动画渲染，26.1 数据驱动的注册与 AI 框架
- **命令**：Brigadier 风格命令树（`/gamemode` `/time` `/give` `/gamerule` `/tp` `/kill`），游戏内 Tab 补全
- **界面**：原版风格 HUD、背包、暂停/选项/存档页面，简体中文与英文双语
- **存档**：自定义多世界存档格式（含校验和），不与 Java 版 NBT 世界互通

## 技术栈

C++20 · Vulkan · GLFW 3.4 · GLM · Vulkan Memory Allocator · miniaudio + stb_vorbis · stb_image

## 构建

需要 Vulkan SDK（含 MoltenVK 与 `glslc`）；macOS 交叉编译 Windows 需先 `brew install mingw-w64`。

```sh
./build.sh              # 一键构建 debug + release 两个分支并组装发布包
```

或分别构建：

```sh
cmake -S . -B build/rebedrock-debug -DBUILD_TESTING=ON
cmake --build build/rebedrock-debug --parallel 8
./build/rebedrock-debug/game/bin/mc_rebedrock
```

`--help` 参数提供隔离方块渲染场景（`--test-scene <方块> --stage <0..9>`），供截图回归与单方块检查。

## 运行所需原版资源

游戏运行需要 Minecraft Java 1.16.1 的原版资源（贴图、音效、字形、语言文件）。这些资源受
Mojang 版权保护，无法随仓库分发，已由 `.gitignore` 排除——需要自行获取并放到源码树
`resources/vanilla/1.16.1/` 下，构建时 CMake 会自动把运行所需子集打包进 `game/resources/`。

### 获取方式一：提取工具（推荐）

从本地 Fabric Loom 缓存提取并自动分类：

```sh
python3 tools/extract_vanilla_resources.py 1.16.1
```

前置要求：工作区同级存在 `mc-26.1-java` 项目，且已执行过 `./gradlew downloadAssets`
（工具读取其 `.gradle/loom-cache` 中的合并 JAR 与资产索引）。产物写入
`resources/vanilla/1.16.1/`，包含 textures、audio、fonts、localization、models、
blockstates、shaders 等全部分类（约 370MB）。

### 获取方式二：手动放置

从你自己的 1.16.1 安装中拷贝以下**运行必需**子集到 `resources/vanilla/1.16.1/`：

```text
resources/vanilla/1.16.1/
├── textures/minecraft/            # 方块/物品/实体/GUI/环境贴图（约 24M）
├── audio/minecraft/sounds/        # 音效（约 2.6M）
│   ├── dig/   step/   liquid/   random/   damage/
│   └── mob/pig/                   # 猪叫，可选；缺失则猪无声
├── fonts/minecraft/               # 字形：glyph_sizes.bin、ascii.png、unicode_page_XX.png
└── localization/minecraft/        # 语言文件
    ├── en_us.json
    └── zh_cn.json
```

> 提取工具生成的其余分类（models / blockstates / data / shaders / particles / texts /
> metadata 等）构建与运行均不需要，仅保留作开发参考。

### 构建与打包

- **源码构建**：放好资源后执行 `./build.sh`。CMake 把上面需要的子集复制进
  `build/<分支>/game/resources/vanilla/1.16.1/`，`game/` 目录即包含全部运行所需资源。
- **预编译发布包**：`./tools/make-release-pak.sh` 生成的
  `build/mc-rebedrock-<版本>-win-mac.zip` 已内嵌全部运行所需资源，收件人解压即可运行，
  无需再提取。
