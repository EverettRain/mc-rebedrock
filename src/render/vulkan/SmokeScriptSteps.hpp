#pragma once

// The scripted client session's STEPS: everything MC_REBEDROCK_SMOKE_TEST does,
// in one place, registered on a SmokeScript (render/SmokeScript.hpp) that the
// render loop then advances with a single call per frame.
//
// This is a test harness, not gameplay. It used to be spliced into the middle of
// VulkanRenderer's frame loop as a 175-line `else if (smokeTest && frame == N)`
// chain, which is why it now lives behind its own door: a release run never
// builds a script at all, and the loop reads as a loop.
//
// It is a template on the host purely so it can live outside VulkanRenderer.cpp
// while still driving the renderer's own members (the Impl type is local to that
// translation unit). There is exactly one instantiation.
//
// What the script covers, in order: the title → options → video → experimental →
// world-list → create-world menu walk; opening a world; the creative catalogue,
// slot clicks and scrolling; the pause menu; a dropped item entity; the debug
// overlay; /gamemode into survival and back (checking the shared inventory
// survives); the damage tint; sun shadows off for the remainder (the default
// players actually run); a block-break particle burst; /give by catalog index and
// by identifier; /time set; creative eating (must NOT spend the food) then
// survival eating (must spend it); rain and then a full storm; /tp; and finally
// the client-cache/world agreement and three-world memory checks before it
// returns to the title and closes the window.

#include "gameplay/GameCommand.hpp"
#include "gameplay/Item.hpp"
#include "gameplay/Inventory.hpp"
#include "persistence/SaveRepository.hpp"
#include "render/SmokeScript.hpp"
#include "ui/PageStack.hpp"
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

// The one piece of state the steps pass between themselves: the apple stack size
// recorded before a meal, so the next step can assert whether it was spent.
struct SmokeScriptState final {
    std::uint8_t appleCount = 0;
};

// Registers every step of the scripted session on `script`.
//
// `stressFrames` is MC_REBEDROCK_STRESS_FRAMES (0 when unset) and `frameLimit`
// the gameplay-frame count the run ends at. `host` is the renderer's Impl.
template <typename Host>
void installSmokeScript(Host& host, SmokeScript& script, std::size_t stressFrames,
                        std::uint64_t frameLimit) {
    const auto state = std::make_shared<SmokeScriptState>();

    // --- The menu walk, on the rendered-frame clock (no world yet). ---

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
        // The 实验性内容 sub-page must open as a menu page (not fall through to
        // the terrain-loading screen) with its five options.
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

    // --- In-world, on the gameplay-frame clock. ---

    script.atGameplayFrame(16, [&host] { host.setInventoryOpen(true); });
    script.atGameplayFrame(20, [&host] {
        // The creative catalogue click goes through the command queue so the
        // smoke exercises the real interaction path.
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
        host.chatInputText = "/gamemode survival";
    });
    script.atGameplayFrame(54, [&host] { host.submitChatInput(); });
    script.atGameplayFrame(56, [&host, &script] {
        // The /gamemode survival command lands on the next server tick.
        script.waitFor("enter survival mode", 40U, [&host] {
            const auto smokeRead = host.worldLock.read();
            return host.clientMirror_.player().gameMode == gameplay::GameMode::Survival;
        });
        if (host.clientMirror_.world().inventorySlots[0].item != &gameplay::items::Diamond) {
            throw std::runtime_error("Smoke test lost the shared inventory during mode switch");
        }
    });
    script.atGameplayFrame(57, [&host] {
        // Exercise the damage-tint draw immediately after the held-item pass.
        // This catches descriptor-set compatibility drift between
        // itemPipelineLayout and hudPipelineLayout under validation.
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
        // Then run the rest of the script with the sun shadows *off*, which is
        // the default every player actually uses. Forcing them on for the whole
        // run left that path unvalidated, and it is the one where the shadow
        // depth map is never rendered into: the descriptors still declare
        // SHADER_READ_ONLY_OPTIMAL and three fragment shaders still sample it. A
        // layout that only held because the pre-pass happened to run is exactly
        // the bug this alternation exists to catch.
        host.options.sunShadows = false;
        host.shadowDisabled = true;
    });
    script.atGameplayFrame(60, [&host] {
        // Deterministically exercise the instanced particle path: a block-break
        // burst next to the player produces hundreds of particles in a single
        // vkCmdDraw.
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
        host.chatInputText = "/gamemode creative";
    });
    script.atGameplayFrame(70, [&host] { host.submitChatInput(); });
    script.atGameplayFrame(72, [&host, &script] {
        // The /gamemode command lands on the next server tick.
        script.waitFor("return to creative mode", 40U, [&host] {
            const auto smokeRead = host.worldLock.read();
            return host.clientMirror_.player().gameMode == gameplay::GameMode::Creative &&
                   host.clientMirror_.world().inventorySlots[0].item == &gameplay::items::Diamond;
        });
    });
    script.atGameplayFrame(74, [&host] {
        host.setChatOpen(true);
        host.chatInputText = "/give 0 1";
    });
    script.atGameplayFrame(76, [&host] { host.submitChatInput(); });
    script.atGameplayFrame(78, [&host, &script] {
        // Catalog index 0 is the first registered building block (grass). The
        // /give command lands on the next server tick.
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
        host.chatInputText = "/give minecraft:acacia_planks 3";
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
        host.chatInputText = "/time set midnight";
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
        // Put a full stack of apples into the last hotbar slot, then close the
        // screen and select it — all through the command queue so the smoke
        // exercises the real interaction path.
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
        // In creative the meal must not spend the food (Java 1.16.1).
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
        host.chatInputText = "/gamemode survival";
    });
    script.atGameplayFrame(404, [&host] { host.submitChatInput(); });
    script.atGameplayFrame(406, [&host, &script, state] {
        // The /gamemode survival command lands on the next server tick. Once the
        // mode lands, arm the meal: the apple survived creative eating, survival
        // should spend it.
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
        // Snap the weather to full rain instantly (test helper, not chat) so the
        // smoke exercises the rain path at full intensity regardless of frame
        // rate; the three render modes compare identical drop counts.
        const auto smokeWrite = host.worldLock.write();
        host.gameSession.weatherSystem().forceRainGradient(1.0F);
    });
    script.atGameplayFrame(420, [&host] {
        // Escalate to a full storm so the smoke also exercises the
        // thunder-boosted rain volume and cross-wind.
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
        host.chatInputText = "/tp 8 200 8";
    });
    script.atGameplayFrame(704, [&host] { host.submitChatInput(); });
    script.atGameplayFrame(706, [&host, &script] {
        // The /tp command lands on the next server tick.
        script.waitFor("/tp teleport", 40U, [&host] {
            const auto smokeRead = host.worldLock.read();
            return host.clientMirror_.player().physicsCurrent.y >= 150.0F;
        });
    });

    // --- The finale: agreement + memory checks, then back to the title. ---

    script.finishWhen(
        [&host, &script, frameLimit] {
            return script.gameplayFrame() >= frameLimit && host.completedStreamBatchCount >= 2U &&
                   host.completedBlockEditCount >= 1U && host.pendingSectionUpdates.empty();
        },
        [&host] {
            // M-Chunk B-5: the spawn chunk must have reached the client cache —
            // an empty cache means the dual-world split lost the chunks and every
            // presentation read (raycast, light, mesh preview) would silently see
            // air.
            if (!host.clientCache.hasChunk(world::chunkPositionFromWorld(24.5F, 24.5F))) {
                throw std::runtime_error("Smoke test: client cache lost the spawn chunk");
            }
            // M-Chunk B-5: the client cache must mirror the server world — same
            // batches, same edits — so the spawn column agrees cell for cell. A
            // broken edit-sync or a missed state delta would diverge them and the
            // render would silently show the wrong world.
            for (int syncY = world::kMinY; syncY < world::kMaxY; ++syncY) {
                if (host.clientCache.state(24, syncY, 24) !=
                    host.interactionWorld.state(24, syncY, 24)) {
                    throw std::runtime_error(
                        "Smoke test: client cache diverged from the server world");
                }
            }
            // Side-split memory: the server world, the client cache AND the
            // streamer worker world each own a chunk copy (three resident copies
            // — the P1-2 debt). All three are measured and the sum is bounded — a
            // gross leak on any side blows it.
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
