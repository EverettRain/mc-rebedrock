#pragma once

// 脚本化客户端会话的全部步骤，MC_REBEDROCK_SMOKE_TEST 要做的事都集中在这里
// 它们注册到一个 SmokeScript 上，由渲染主循环每帧调一次推进，调度器见 render/SmokeScript.hpp
//
// 这是测试脚手架而非玩法，所以单独成文件：正式运行根本不会构造脚本，主循环也就还是一个纯粹的循环
// 做成模板只是为了住在 VulkanRenderer.cpp 之外还能驱动渲染器自己的成员
// 因为 Impl 是那个翻译单元的局部类型，实际只有一处实例化
//
// 剧本按以下顺序覆盖各条路径：
//   * 标题 → 选项 → 视频设置 → 实验性内容 → 世界列表 → 创建世界的菜单走查
//   * 开世界，然后是创造目录、槽位点击与滚动、暂停菜单
//   * 掉落物实体与调试叠加层
//   * /gamemode 切到生存再切回，顺带确认共享背包没丢
//   * 受伤染色，此后关闭太阳阴影，那才是玩家的默认配置
//   * 方块破坏粒子爆发
//   * 按目录下标和按标识符两种 /give，以及 /time set
//   * 创造模式进食不该消耗食物，生存模式进食必须消耗
//   * 下雨再升级为雷暴，然后 /tp
//   * 最后校验客户端缓存与服务端世界逐格一致、三份世界的常驻内存在预算内，再返回标题并关窗

#include "gameplay/GameCommand.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/Inventory.hpp"
#include "persistence/SaveRepository.hpp"
#include "render/SmokeScript.hpp"
#include "ui/PageStack.hpp"
#include "ui/TextField.hpp"
#include "world/Block.hpp"
#include "world/ChunkStreamer.hpp"
#include "world/WorldConstants.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace mc::render {

// 步骤之间唯一需要传递的状态：进食前记下的苹果堆数量，供后续步骤断言它有没有被消耗
struct SmokeScriptState final {
    std::uint8_t appleCount = 0;
};

// 把脚本会话的所有步骤注册到 `script` 上
// `stressFrames` 即 MC_REBEDROCK_STRESS_FRAMES，未设置时为 0
// `frameLimit` 是本次运行收尾的游戏帧数，`host` 是渲染器的 Impl
template <typename Host>
void installSmokeScript(Host& host, SmokeScript& script, std::size_t stressFrames,
                        std::uint64_t frameLimit) {
    const auto state = std::make_shared<SmokeScriptState>();

    // ---- 菜单走查，跑在渲染帧时钟上（此时还没有世界）----

    script.atRenderedFrame(2, [&host] {
        if (host.menuSystem.pageStack.current() != ui::PageId::Title) {
            throw std::runtime_error("Smoke test did not start at title page");
        }
        host.menuSystem.optionsOpen = true;
        host.menuSystem.pageStack.push(ui::PageId::Options);
    });
    script.atRenderedFrame(3, [&host] { host.menuSystem.pageStack.push(ui::PageId::VideoSettings); });
    script.atRenderedFrame(4, [&host] {
        if (host.menuSystem.pageStack.current() != ui::PageId::VideoSettings) {
            throw std::runtime_error("Smoke test did not open video settings");
        }
        host.menuSystem.pageStack.pop();
        // 实验性内容子页必须作为菜单页打开（不能掉到地形加载画面），且带五个选项
        host.menuSystem.pageStack.push(ui::PageId::Experimental);
        if (host.menuSystem.pageStack.current() != ui::PageId::Experimental ||
            host.menuButtonCount() != 5U) {
            throw std::runtime_error("Smoke test experimental content page failed");
        }
        host.menuSystem.pageStack.pop();
        host.menuSystem.optionsOpen = false;
        host.menuSystem.pageStack.pop();
        host.menuSystem.pageStack.push(ui::PageId::WorldList);
    });
    script.atRenderedFrame(6, [&host] { host.menuSystem.pageStack.push(ui::PageId::CreateWorld); });
    script.atRenderedFrame(8, [&host, &script] {
        persistence::SaveGame smokeWorld;
        smokeWorld.summary.displayName = "Smoke Test";
        smokeWorld.summary.seed = 0x5EEDULL;
        gameplay::Inventory smokeInventory;
        smokeWorld.inventory = smokeInventory.slots();
        smokeWorld.selectedHotbarSlot = smokeInventory.selectedHotbarSlot();
        host.startWorld(std::move(smokeWorld));
        script.markWorldStarted();
    });

    // ---- 进入世界之后，跑在游戏帧时钟上 ----

    script.atGameplayFrame(16, [&host] { host.setInventoryOpen(true); });
    script.atGameplayFrame(20, [&host] {
        // 创造目录的点击走命令队列，让烟测经过真实的交互路径
        gameplay::ClickCreativeItem creative;
        creative.catalogStack = {world::Block::Air, 1U, &gameplay::items::Diamond};
        creative.button = gameplay::InventoryMouseButton::Left;
        host.runtime.enqueueClientCommand(std::move(creative));
    });
    script.atGameplayFrame(24, [&host] {
        gameplay::ClickSlot click;
        click.kind = gameplay::SlotKind::PlayerInventory;
        click.slotIndex = 0U;
        click.button = 0;
        host.runtime.enqueueClientCommand(std::move(click));
    });
    script.atGameplayFrame(28, [&host] {
        const auto smokeRead = host.worldLock.read();
        host.scrollCreative(1);
    });
    script.atGameplayFrame(32, [&host] { host.setInventoryOpen(false); });
    script.atGameplayFrame(34, [&host] { host.setPaused(true); });
    script.atGameplayFrame(36, [&host] {
        host.menuSystem.optionsOpen = true;
        host.menuSystem.pageStack.push(ui::PageId::Options);
    });
    script.atGameplayFrame(38, [&host] {
        host.menuSystem.optionsOpen = false;
        host.menuSystem.pageStack.pop();
        host.setPaused(false);
    });
    script.atGameplayFrame(40, [&host] {
        const auto smokeWrite = host.worldLock.write();
        host.gameSession.spawnItemEntity(
            host.clientMirror_.player().physicsCurrent + glm::vec3{1.8F, 1.0F, 0.0F},
            {world::Block::DiamondOre, 3}, {0.0F, 0.12F, 0.0F});
    });
    script.atGameplayFrame(44, [&host] { host.debugOverlayOpen = true; });
    script.atGameplayFrame(48, [&host] { host.debugOverlayOpen = false; });
    script.atGameplayFrame(50, [&host] {
        host.setChatOpen(true);
        host.chatInput =
            mc::ui::textFieldWithValue("/gamemode survival", mc::ui::kChatFieldRules, {});
    });
    script.atGameplayFrame(54, [&host] { host.submitChatInput(); });
    script.atGameplayFrame(56, [&host, &script] {
        // /gamemode survival 要到下一个服务端 tick 才生效
        script.waitFor("enter survival mode", 40U, [&host] {
            const auto smokeRead = host.worldLock.read();
            return host.clientMirror_.player().gameMode == gameplay::GameMode::Survival;
        });
        if (host.clientMirror_.world().inventorySlots[0].item != &gameplay::items::Diamond) {
            throw std::runtime_error("Smoke test lost the shared inventory during mode switch");
        }
    });
    script.atGameplayFrame(57, [&host] {
        // 紧接手持物品通道之后触发受伤染色绘制
        // 用于在校验层下抓出 itemPipelineLayout 与 hudPipelineLayout 描述符集兼容性的漂移
        const auto smokeWrite = host.worldLock.write();
        if (!host.gameSession.hurtPlayer(gameplay::kPrimaryPlayerId,
                                         gameplay::DamageType::Generic, 1.0F, host)) {
            throw std::runtime_error("Smoke test could not trigger damage overlay");
        }
    });
    script.atGameplayFrame(58, [&host] { host.setInventoryOpen(true); });
    script.atGameplayFrame(59, [&host, stressFrames] {
        if (stressFrames > 0U) {
            return;
        }
        // 剩下的剧本改在太阳阴影*关闭*下跑——那才是玩家的默认配置
        // 全程开着会漏掉这条路径，而恰恰是它没有任何东西画进阴影深度图
        // 那时描述符仍声明 SHADER_READ_ONLY_OPTIMAL，三个片元着色器也仍在采样它
        // "布局之所以成立只是因为预通道碰巧跑了"正是这次切换要抓的 bug
        host.options.sunShadows = false;
        host.shadowDisabled = true;
    });
    script.atGameplayFrame(60, [&host] {
        // 确定性地走一遍实例化粒子路径：玩家脚边的破坏粒子爆发会在单次 vkCmdDraw 里产出几百个粒子
        const auto smokeRead = host.worldLock.read();
        const glm::vec3 spawn = host.clientMirror_.player().physicsCurrent;
        host.particleSystem.spawnBlockBreak({static_cast<int>(std::floor(spawn.x)),
                                             static_cast<int>(std::floor(spawn.y)) - 2,
                                             static_cast<int>(std::floor(spawn.z))},
                                            world::Block::Dirt);
    });
    script.atGameplayFrame(62, [&host] { host.setInventoryOpen(false); });
    script.atGameplayFrame(66, [&host] {
        host.setChatOpen(true);
        host.chatInput =
            mc::ui::textFieldWithValue("/gamemode creative", mc::ui::kChatFieldRules, {});
    });
    script.atGameplayFrame(70, [&host] { host.submitChatInput(); });
    script.atGameplayFrame(72, [&host, &script] {
        // /gamemode 要到下一个服务端 tick 才生效
        script.waitFor("return to creative mode", 40U, [&host] {
            const auto smokeRead = host.worldLock.read();
            return host.clientMirror_.player().gameMode == gameplay::GameMode::Creative &&
                   host.clientMirror_.world().inventorySlots[0].item == &gameplay::items::Diamond;
        });
    });
    script.atGameplayFrame(74, [&host] {
        host.setChatOpen(true);
        host.chatInput =
            mc::ui::textFieldWithValue("/give 0 1", mc::ui::kChatFieldRules, {});
    });
    script.atGameplayFrame(76, [&host] { host.submitChatInput(); });
    script.atGameplayFrame(78, [&host, &script] {
        // 目录下标 0 是第一个注册的建筑方块（草方块）
        // /give 要到下一个服务端 tick 才生效
        script.waitFor("/give by catalog index", 40U, [&host] {
            const auto smokeRead = host.worldLock.read();
            return std::ranges::any_of(host.clientMirror_.world().inventorySlots,
                                       [](const gameplay::ItemStack& stack) {
                                           return stack.block == world::Block::Grass &&
                                                  stack.count >= 1U;
                                       });
        });
    });
    script.atGameplayFrame(80, [&host] {
        host.setChatOpen(true);
        host.chatInput =
            mc::ui::textFieldWithValue("/give minecraft:acacia_planks 3", mc::ui::kChatFieldRules, {});
    });
    script.atGameplayFrame(82, [&host] { host.submitChatInput(); });
    script.atGameplayFrame(84, [&host, &script] {
        script.waitFor("/give by identifier", 40U, [&host] {
            const auto smokeRead = host.worldLock.read();
            return std::ranges::any_of(host.clientMirror_.world().inventorySlots,
                                       [](const gameplay::ItemStack& stack) {
                                           return stack.block == world::Block::AcaciaPlanks &&
                                                  stack.count >= 3U;
                                       });
        });
    });
    script.atGameplayFrame(86, [&host] {
        host.setChatOpen(true);
        host.chatInput =
            mc::ui::textFieldWithValue("/time set midnight", mc::ui::kChatFieldRules, {});
    });
    script.atGameplayFrame(88, [&host] { host.submitChatInput(); });
    script.atGameplayFrame(90, [&host, &script] {
        script.waitFor("set world time", 40U, [&host] {
            const auto smokeRead = host.worldLock.read();
            const auto perDay = static_cast<std::uint64_t>(world::DayNightCycle::kTicksPerDay);
            const auto tick = std::fmod(host.clientMirror_.world().dayTimeTicks,
                                        static_cast<double>(perDay));
            return std::abs(tick - 18000.0) <= 4.0;
        });
    });
    script.atGameplayFrame(92, [&host] { host.setInventoryOpen(true); });
    script.atGameplayFrame(94, [&host] {
        // 往快捷栏最后一格放一整堆苹果，然后关界面并选中它
        // 全程走命令队列，让烟测经过真实的交互路径
        gameplay::ClickCreativeItem creative;
        creative.catalogStack = {world::Block::Air, 1U, &gameplay::items::Apple};
        creative.button = gameplay::InventoryMouseButton::Left;
        host.runtime.enqueueClientCommand(std::move(creative));
        gameplay::ClickSlot click;
        click.kind = gameplay::SlotKind::PlayerInventory;
        click.slotIndex = 8U;
        click.button = 0;
        host.runtime.enqueueClientCommand(std::move(click));
        const auto smokeWrite = host.worldLock.write();
        host.setInventoryOpenLocked(false);
        gameplay::SwapSlot swap;
        swap.index = 8U;
        host.runtime.enqueueClientCommand(std::move(swap));
    });
    script.atGameplayFrame(96, [&host, state] {
        const auto smokeRead = host.worldLock.read();
        state->appleCount =
            host.clientMirror_.world()
                .inventorySlots[host.clientMirror_.player().selectedHotbarSlot]
                .count;
        if (state->appleCount == 0U) {
            throw std::runtime_error("Smoke test apple stack missing");
        }
        // 创造模式下进食不消耗食物
        host.enqueueUseStart();
    });
    script.atGameplayFrame(400, [&host, state] {
        host.enqueueUseStop();
        const auto smokeRead = host.worldLock.read();
        if (host.clientMirror_.world()
                .inventorySlots[host.clientMirror_.player().selectedHotbarSlot]
                .count != state->appleCount) {
            throw std::runtime_error("Smoke test creative eating consumed food");
        }
    });
    script.atGameplayFrame(402, [&host] {
        host.setChatOpen(true);
        host.chatInput =
            mc::ui::textFieldWithValue("/gamemode survival", mc::ui::kChatFieldRules, {});
    });
    script.atGameplayFrame(404, [&host] { host.submitChatInput(); });
    script.atGameplayFrame(406, [&host, &script, state] {
        // /gamemode survival 要到下一个服务端 tick 才生效
        // 模式落地后再安排进食：苹果在创造模式下没被吃掉，生存模式必须吃掉它
        script.waitFor(
            "return to survival mode", 40U,
            [&host] {
                const auto smokeRead = host.worldLock.read();
                return host.clientMirror_.player().gameMode == gameplay::GameMode::Survival;
            },
            [&host, state] {
                gameplay::SwapSlot swap;
                swap.index = 8U;
                host.runtime.enqueueClientCommand(std::move(swap));
                state->appleCount = host.clientMirror_.world().inventorySlots[8U].count;
                if (state->appleCount == 0U) {
                    throw std::runtime_error("Smoke test apple stack missing in survival");
                }
                host.enqueueUseStart();
            });
    });
    script.atGameplayFrame(410, [&host] {
        // 直接把天气拨到满雨（走测试接口而非聊天命令），使烟测无论帧率如何都以满强度走一遍降雨路径
        // 三种渲染模式因此比较的是同样的雨滴数
        const auto smokeWrite = host.worldLock.write();
        host.gameSession.weatherSystem().forceRainGradient(1.0F);
    });
    script.atGameplayFrame(420, [&host] {
        // 再升级为雷暴，顺带覆盖雷暴加强后的雨量与横风
        const auto smokeWrite = host.worldLock.write();
        host.gameSession.weatherSystem().forceThunderGradient(1.0F);
    });
    script.atGameplayFrame(700, [&host, state] {
        host.enqueueUseStop();
        const auto smokeRead = host.worldLock.read();
        if (host.clientMirror_.world()
                .inventorySlots[host.clientMirror_.player().selectedHotbarSlot]
                .count >= state->appleCount) {
            throw std::runtime_error("Smoke test survival eating did not consume an apple");
        }
    });
    script.atGameplayFrame(702, [&host] {
        host.setChatOpen(true);
        host.chatInput =
            mc::ui::textFieldWithValue("/tp 8 200 8", mc::ui::kChatFieldRules, {});
    });
    script.atGameplayFrame(704, [&host] { host.submitChatInput(); });
    script.atGameplayFrame(706, [&host, &script] {
        // /tp 要到下一个服务端 tick 才生效
        script.waitFor("/tp teleport", 40U, [&host] {
            const auto smokeRead = host.worldLock.read();
            return host.clientMirror_.player().physicsCurrent.y >= 150.0F;
        });
    });

    // ---- 收尾：一致性与内存校验，然后返回标题 ----

    script.finishWhen(
        [&host, &script, frameLimit] {
            return script.gameplayFrame() >= frameLimit && host.completedStreamBatchCount >= 2U &&
                   host.completedBlockEditCount >= 1U && host.pendingSectionUpdates.empty();
        },
        [&host] {
            // 出生点区块必须已经进到客户端缓存
            // 缓存为空说明双世界拆分把区块弄丢了
            // 之后射线、光照、网格预览等所有表现侧读取都会静默地读到空气
            if (!host.clientCache.hasChunk(world::chunkPositionFromWorld(24.5F, 24.5F))) {
                throw std::runtime_error("Smoke test: client cache lost the spawn chunk");
            }
            // 客户端缓存必须与服务端世界一致（同样的批次、同样的编辑），出生点那一列因此逐格相同
            // 编辑同步坏掉或漏了一次状态增量都会让两边分叉，而画面会毫无声息地显示一个错的世界
            for (int syncY = world::kMinY; syncY < world::kMaxY; ++syncY) {
                if (host.clientCache.state(24, syncY, 24) !=
                    host.interactionWorld.state(24, syncY, 24)) {
                    throw std::runtime_error(
                        "Smoke test: client cache diverged from the server world");
                }
            }
            // 双端拆分下同一区块存在三份常驻副本：服务端世界、客户端缓存、流送工作线程的世界
            // 三份都测量并对总量设上限，任一侧的大泄漏都会把它撑爆
            const auto serverChunkBytes = host.interactionWorld.residentBytes();
            const auto clientChunkBytes = host.clientCache.residentBytes();
            const auto workerChunkBytes = host.chunkStreamer.workerWorldResidentBytes();
            std::cout << "[memory] serverChunkResident=" << serverChunkBytes
                      << " clientChunkResident=" << clientChunkBytes
                      << " workerChunkResident=" << workerChunkBytes
                      << " total=" << (serverChunkBytes + clientChunkBytes + workerChunkBytes)
                      << "\n";
            if (serverChunkBytes + clientChunkBytes + workerChunkBytes > 512U * 1024U * 1024U) {
                throw std::runtime_error("Smoke test: three-world resident exceeds the budget");
            }
            host.returnToTitle(false);
        },
        [&host] { glfwSetWindowShouldClose(host.window, GLFW_TRUE); });
}

} // namespace mc::render
