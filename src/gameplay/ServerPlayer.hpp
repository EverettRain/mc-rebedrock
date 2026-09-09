#pragma once

// One connected player's authoritative state, the way 26.1's ServerPlayer owns
// its controller, inventory, vitals and action timeline. GameSession holds a
// slot map of these indexed by a stable PlayerId (the same shape EntitySystem
// uses: the id survives vector compaction). Today ReBedrock has a single local
// player, so the map has one entry; the structure exists so N2's multi-player
// and N3's snapshot can address a player by id without renaming the state.
//
// The three PlayerInput copies are kept together here: `stagedInput` is the
// render thread's write, `sharedInput` the hand-off under the input mutex, and
// `playerInput` the simulation's own snapshot at the top of each tick.

#include "gameplay/CraftingSystem.hpp"
#include "gameplay/Anvil.hpp"
#include "gameplay/EnchantingTable.hpp"
#include "gameplay/Equipment.hpp"
#include "gameplay/GameMode.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/PlayerActionState.hpp"
#include "gameplay/PlayerController.hpp"
#include "gameplay/PlayerExperience.hpp"
#include "gameplay/PlayerVitals.hpp"
#include "gameplay/PlayerAdvancements.hpp"
#include "gameplay/RecipeBook.hpp"

#include <glm/vec3.hpp>

#include <cstdint>

namespace mc::gameplay {

// A stable identity for a connected player. The single local player uses
// kPrimaryPlayerId; remote players (LAN/dedicated, C-tier) get fresh ids.
using PlayerId = std::uint64_t;
inline constexpr PlayerId kPrimaryPlayerId = 1U;

struct ServerPlayer final {
    // PlayerController has no default constructor (it needs the spawn feet), so
    // neither does a player without a position.
    explicit ServerPlayer(glm::vec3 feet) : controller(feet) {}

    PlayerController controller;
    PlayerVitals vitals;
    // XP-0: the level/points/total/enchantmentSeed currency state, parallel to
    // vitals rather than folded into it (Player keeps these as siblings of
    // health/food in 26.1 too — different lifetime: a respawn resets vitals
    // but never experience).
    PlayerExperience experience;
    Inventory inventory;
    // EQ-0: the four armor slots + offhand, a small companion to `inventory`
    // rather than folded into its slot array (see Equipment.hpp's banner).
    // Mainhand stays inventory.selectedStack() — not duplicated here.
    EquipmentSlots equipment;
    CraftingSystem crafting;
    // 配方书：已解锁 / 待高亮两个集合。26.1 里 `ServerRecipeBook` 也是挂在
    // ServerPlayer 上的（ServerPlayer.java:1485-1488 的 `this.recipeBook`），
    // 因为它是**每个玩家一份**的进度，不是世界状态。
    RecipeBook recipeBook;
    // ADV-1：成就进度，与配方书同一个理由挂在玩家身上（26.1 的
    // `ServerPlayer.advancements` 也是每个玩家一份）。配方解锁链就是「成就完成
    // -> rewards.recipes -> recipeBook」这条路，所以两者必然同层。
    PlayerAdvancements advancements;
    // 上一 tick 的背包槽指纹，inventory_changed 靠它分辨「哪一槽变了」。
    // 纯运行期缓存，不落盘（落盘的是 advancements 里的已完成 criterion）。
    std::vector<InventorySlotFingerprint> inventoryFingerprints;
    // ENCH-2: the open enchanting screen's two input slots and derived offers.
    // A sibling of `crafting` for the same reason: vanilla's EnchantmentMenu,
    // like the crafting grid, is menu-scoped state the PLAYER carries and hands
    // back on close — not block-entity state, so it needs no per-table storage
    // and no save-format change.
    EnchantingMenu enchanting;
    // ENCH-3: the open anvil's two inputs and derived result. Menu-scoped for
    // the same reason `enchanting` is — vanilla's ItemCombinerMenu owns its
    // inputs and returns them in removed(), so the anvil block stores nothing.
    AnvilMenu anvil;
    GameMode gameMode = GameMode::Creative;

    // The tick-owned swing/use timeline (N1), advanced with the world tick.
    PlayerActionState actions;

    // Three input copies on purpose: staged (render thread), shared (hand-off
    // under the mutex), simulation's own (refreshed each tick).
    PlayerInput stagedInput{};
    PlayerInput sharedInput{};
    PlayerInput playerInput{};

    // Interpolation endpoints the renderer draws between (physics partialTick).
    glm::vec3 physicsPrevious{0.0F};
    glm::vec3 physicsCurrent{0.0F};

    // The /spawnpoint result; death respawns here before the world spawn.
    glm::vec3 spawnPosition{24.0F, 76.38F, 24.0F};
    float spawnYaw = 0.0F;
    bool hasSpawn = false;

    float footstepDistance = 0.0F;
    bool previousInWater = false;

    // The vanilla 32-tick meal (N1's ItemUseState mirrors it for the animation).
    // SLP-2: Player#isSleeping / #sleepCounter. The bed cell is kept so waking
    // can clear OCCUPIED on the right block, and the counter is what
    // isSleepingLongEnough (100 ticks) reads.
    bool sleeping = false;
    glm::ivec3 sleepingIn{0};
    int sleepTicks = 0;

    bool eating = false;
    const Item* eatingKind = nullptr;
    int eatTicks = 0;
};

} // namespace mc::gameplay
