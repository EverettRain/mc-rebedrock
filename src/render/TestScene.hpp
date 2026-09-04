#pragma once

// 测试场景的命令行开关
// 渲染回归用：把世界换成一个内容确定的小场景，截图因此能逐帧对比
// 场景可以是单个方块的各生长阶段，也可以是受控的遮挡场景
// 正常游戏不经过这里

#include "world/Block.hpp"
#include "world/BlockState.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mc::render {

struct TestSceneOptions final {
    world::Block block = world::Block::Stone;
    // RN-15c: the whole state, not just the block. `--stage` can only rotate the
    // six orientations, and the properties this line most needs to look at are
    // open/half/hinge/powered/delay/locked/in_wall — so the scene spec accepts
    // `oak_trapdoor[open=true,half=top]` and this carries the result.
    world::BlockState state{world::Block::Stone};
    // Whether the spec named `facing` itself. When it did, `--stage` must not
    // also spin the block: the two would fight and the picture would silently
    // not be the state that was asked for.
    bool stateSetsFacing = false;
    // The properties as they were spelled on the command line, in order. Only
    // used to name the output directory, so the same command always writes the
    // same path (RN-15's determinism rule reaches the file names too).
    std::vector<std::string> stateSpec;
    int stage = 0;
    // 渲染一个受控的遮挡测试场景：平整石台加埋在下面的洞穴，再开一个地表开口
    // 这样遮挡查询的结果是可预期的
    bool occlusionScene = false;

    // RN-15d: render the block from the eight corner viewpoints, write one PNG
    // each under `previewRoot`, and exit. Non-zero exit if any one of them fails
    // — seven images out of eight, silently, is the worst outcome for something
    // an automation diffs.
    bool exportPreview = false;
    // Square, and fixed rather than taken from the window: an export whose size
    // depends on the monitor it ran on cannot be compared with one from another
    // machine, and RN-15 is a comparison tool before it is anything else.
    std::uint32_t previewSize = 512U;
    std::filesystem::path previewRoot{"export/blocks-preview"};

    [[nodiscard]] bool operator==(const TestSceneOptions&) const = default;
};

// The directory one export writes into: the block's identifier with `:` replaced
// by `_`, plus one `__<property>-<value>` segment per property the spec named, in
// the order it named them.
//
// `:` is legal in a POSIX path and not on Windows, and this project ships both;
// replacing it is the choice RN-15b records rather than dropping the namespace,
// because a datapack block one day sharing a path with a built-in would otherwise
// overwrite its pictures.
[[nodiscard]] std::string previewDirectoryName(const TestSceneOptions& options);

// 命令行形式为 --test-scene <方块规格> [--stage <0..9>]
// 方块规格是 `<数字 id|minecraft:id|裸名>`，可选带 `[属性=值,...]`
// 另有 --occlusion-scene 选受控遮挡场景
// RN-15d 追加 --export-preview [--preview-size N] [--preview-out <目录>]
// 没有 --test-scene 时返回 nullopt
// 参数写错直接抛，免得自动化跑着跑着悄悄渲染了错误的资源还当成功
[[nodiscard]] std::optional<TestSceneOptions> parseTestSceneArguments(
    std::span<const std::string_view> arguments);

} // namespace mc::render
