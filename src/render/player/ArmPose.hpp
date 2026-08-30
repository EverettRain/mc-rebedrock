#pragma once

// 本帧一只手臂摆出的姿态族，对应 26.1 的 HumanoidModel.ArmPose
// 一只手臂显示哪种姿态是持有物与当前使用动作的纯函数，推导见 deriveArmPose
// 第三人称的身体求解器与第一人称的手部求解器因此读同一个值，而不是各自从物品去猜
//
// 这里不含 Vulkan
// 它虽然是渲染侧的表现词汇，却只依赖玩法层的物品与使用动作身份
// 因此放在运行时库里，可以脱离图形环境做测试

#include "gameplay/Inventory.hpp"
#include "gameplay/PlayerActionState.hpp"

#include <cstdint>

namespace mc::render::player {

enum class ArmPose : std::uint8_t {
    Empty,          // 空手，只有待机摆动与行走摆臂
    Item,           // 平举的扁平物品或工具
    Block,          // 可放置的方块，握持姿势略有不同
    Eat,            // 把食物或饮品举向嘴边，使用进行中
    Bow,            // 拉弓，预留，内容尚未接入
    Spear,          // 蓄力三叉戟，预留
    Crossbow,       // 持弩或给弩上弦，预留
    Spyglass,       // 望远镜举到眼前，预留
    Horn,           // 山羊角，预留
    Brush,          // 刷子，预留
};

// 给持有 stack 的那只手推导手臂姿态，usingThisHand 表示这只手是不是正在使用物品的那只
// 这是一个纯函数且覆盖全部输入，空手是 Empty，方块是 Block，使用中的食物或饮品是 Eat
// 其余一律是 Item
// 预留的那些姿态要等对应内容落地后才会被映射
// 在那之前推导永远不会产出它们，因此两个求解器都不需要写死分支
[[nodiscard]] inline ArmPose deriveArmPose(const gameplay::ItemStack& stack, bool usingThisHand,
                                           gameplay::UseAnimation use) noexcept {
    if (usingThisHand && (use == gameplay::UseAnimation::Eat ||
                          use == gameplay::UseAnimation::Drink)) {
        return ArmPose::Eat;
    }
    if (stack.empty()) {
        return ArmPose::Empty;
    }
    if (gameplay::isBlockStack(stack)) {
        return ArmPose::Block;
    }
    return ArmPose::Item;
}

}  // namespace mc::render::player
