#pragma once

// The render-visible world state, published once per simulation tick under the
// world write lock (the same pattern as PlayerTickSnapshot): the weather, the
// time of day, the named clocks and the game rules the renderer's sky/rain/HUD
// read. The render thread samples this snapshot once per frame instead of
// reaching into live gameplay systems mid-tick.
//
// N3b covers the scalar world state; the block-entity mirror (chests, furnaces)
// and the block deltas ride in their own channels.

#include "gameplay/ChestSystem.hpp"
#include "gameplay/CraftingSystem.hpp"
#include "gameplay/FurnaceSystem.hpp"
#include "gameplay/Inventory.hpp"
#include "gameplay/ScreenHandler.hpp"
#include "gameplay/WeatherSystem.hpp"
#include "world/WorldClock.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace mc::gameplay {

struct WorldSnapshot final {
    // The resident bytes this snapshot holds: the fixed struct (all scalar and
    // inline-array state) plus the chest render states' buffer. Deliberately
    // bounded and independent of world size — this is an incremental mirror of
    // the render-visible world state, not a chunk/block copy (a whole-world
    // snapshot at radius 32 would be hundreds of MB). The N-Mem budget gate
    // pins a per-tick ceiling on it.
    [[nodiscard]] std::size_t residentBytes() const {
        return sizeof(*this) + chests.capacity() * sizeof(ChestRenderState);
    }

    [[nodiscard]] friend bool operator==(const WorldSnapshot&, const WorldSnapshot&) = default;

    std::uint64_t serverTick = 0U;

    // The smoothed weather gradients the sky, rain and thunder render from. The
    // previous/current endpoints let the renderer reproduce the per-frame
    // interpolation rainGradientAt(alpha) would give without the live system.
    float previousRainGradient = 0.0F;
    float rainGradient = 0.0F;
    float previousThunderGradient = 0.0F;
    float thunderGradient = 0.0F;
    bool raining = false;
    bool thundering = false;

    // The overworld time-of-day in ticks [0, 24000), for the sun, moon and the
    // day/night sky.
    double dayTimeTicks = 0.0;

    // The named clocks, so a frozen sun (doDaylightCycle=false) renders still
    // and the day count (moon phase) is preserved.
    std::array<world::ClockState, world::kClockCount> clocks{};

    // The game rules the renderer reads (daylight, weather cycles).
    bool doDaylightCycle = true;
    bool doWeatherCycle = true;

    // The spawn points and the player's personal spawn, for the death/respawn
    // screen and the F3/world-render reads.
    glm::vec3 worldSpawnPosition{24.0F, 76.38F, 24.0F};
    glm::vec3 playerSpawnPosition{24.0F, 76.38F, 24.0F};
    float playerSpawnYaw = 0.0F;
    bool hasPlayerSpawn = false;

    // The authoritative menu binding. The render thread uses this value mirror
    // for hit-testing and HUD configuration instead of reading the live session
    // fields while the simulation may open a container.
    ContainerScreen openContainerScreen = ContainerScreen::PlayerInventory;
    std::optional<ChestPosition> openChest;
    std::optional<glm::ivec3> openFurnace;

    // The chest block entities' render state (position + lid hinge), so the
    // world renderer draws the lid without reaching into the live chest system.
    struct ChestRenderState final {
        ChestPosition position{};
        float previousLidAngle = 0.0F;
        float lidAngle = 0.0F;
        [[nodiscard]] friend bool operator==(const ChestRenderState&, const ChestRenderState&) =
            default;
    };
    std::vector<ChestRenderState> chests;

    // The container screen's display state — the player's inventory and cursor
    // plus the open container's contents — published per tick so the HUD draws
    // the slots from a snapshot instead of live gameplay. Values, not pointers,
    // so no reference into a gameplay vector survives the tick boundary.
    std::array<ItemStack, Inventory::kSlotCount> inventorySlots{};
    ItemStack cursorStack{};
    std::array<ItemStack, ChestBlockEntity::kSlotCount> chestItems{};
    std::array<ItemStack, 9> tableCraftingGrid{};
    ItemStack tableCraftingOutput{};
    std::array<ItemStack, 4> playerCraftingGrid{};
    ItemStack playerCraftingOutput{};
    ItemStack furnaceInput{};
    ItemStack furnaceFuel{};
    ItemStack furnaceOutput{};
    float furnaceFuelProgress = 0.0F;
    float furnaceCookProgress = 0.0F;
};

} // namespace mc::gameplay
