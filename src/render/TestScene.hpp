#pragma once

// 测试场景的命令行开关
// 渲染回归用：把世界换成一个内容确定的小场景，截图因此能逐帧对比
// 场景可以是单个方块的各生长阶段，也可以是受控的遮挡场景
// 正常游戏不经过这里

#include "world/Block.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace mc::render {

struct TestSceneOptions final {
    world::Block block = world::Block::Stone;
    int stage = 0;
    // 渲染一个受控的遮挡测试场景：平整石台加埋在下面的洞穴，再开一个地表开口
    // 这样遮挡查询的结果是可预期的
    bool occlusionScene = false;

    [[nodiscard]] bool operator==(const TestSceneOptions&) const = default;
};

// 命令行形式为 --test-scene <数字 id|minecraft:id> [--stage <0..9>]
// 另有 --occlusion-scene 选受控遮挡场景
// 没有 --test-scene 时返回 nullopt
// 参数写错直接抛，免得自动化跑着跑着悄悄渲染了错误的资源还当成功
[[nodiscard]] std::optional<TestSceneOptions> parseTestSceneArguments(
    std::span<const std::string_view> arguments);

} // namespace mc::render
