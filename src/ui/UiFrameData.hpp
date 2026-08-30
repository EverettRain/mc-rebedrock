#pragma once

#include "gameplay/GameMode.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/ScreenHandler.hpp"

#include <cstddef>
#include <optional>

namespace mc::gameplay {
class GameSession;
}

namespace mc::ui {

// 游戏内 HUD 要画的那部分玩法状态，每帧从会话取一次
// 各绘制通道读的因此是一份一致的快照，而不是在半帧中途去戳活的玩法对象
// 容器槽位刻意不复制，指背包格与合成/熔炉面板：它们本来就照原样绘制
// 每帧复制每个槽位只会把面板正在显示的那份状态再抄一遍
// 只有 HUD 抬头就能看到的那几项走快照，即状态值、手持物品堆和游戏模式
struct UiFrameData final {
    float health = 0.0F;
    int foodLevel = 0;
    int airTicks = 0;
    int ticksSinceDamage = 1000;
    // 经验条的填充比例，以及它上方的等级数字
    int experienceLevel = 0;
    float experienceProgress = 0.0F;
    gameplay::GameMode gameMode = gameplay::GameMode::Survival;
    bool eating = false;
    gameplay::ItemStack selectedStack{};
    std::size_t selectedHotbarSlot = 0;
    gameplay::ContainerScreen containerScreen = gameplay::ContainerScreen::PlayerInventory;
    std::optional<gameplay::ChestPosition> activeChest;
};

} // namespace mc::ui
