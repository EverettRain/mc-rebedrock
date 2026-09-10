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
#include "gameplay/Equipment.hpp"
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

// AR-M6: how many trade offers ride the snapshot. Sized to the largest table
// this build has, and fixed so the snapshot stays a POD with a fixed-width wire
// form. A villager with more offers than this shows the first
// kSnapshotTradeOffers of them.
inline constexpr std::size_t kSnapshotTradeOffers = 8U;
// The "no row picked" value for `tradeSelectedOffer`. 0xFF rather than -1 so the
// field stays a plain byte on the wire.
inline constexpr std::uint8_t kNoSelectedTradeOffer = 0xFFU;

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
    // EQ-0: the four armor slots + offhand, so a future armor renderer (and
    // today's headless mirror test) can read what is worn from the same
    // per-tick snapshot the rest of the container display state rides in.
    // No UI reads this yet — that is the explicit EQ-1/PX seam this node
    // leaves clean, not this node's job.
    std::array<ItemStack, kEquipmentSlotCount> equipmentSlots{};
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
    // ENCH-2: the enchanting screen's display state. The three bars each show a
    // required level, a "clue" enchantment id + level (revealed on hover, the
    // rest of the offer stays hidden as in vanilla) and nothing else; a
    // requiredLevel of 0 is a dead bar. `enchantingBookshelfPower` is the
    // scanned shelf count, unclamped, so the UI can also say "more shelves than
    // the table can use".
    ItemStack enchantingItem{};
    ItemStack enchantingLapis{};
    std::array<std::int32_t, 3> enchantingRequiredLevels{};
    // The clue's raw EnchantmentId storage value, and its level. A level of 0
    // means the bar has no clue to show.
    std::array<std::uint8_t, 3> enchantingClueIds{};
    std::array<std::uint8_t, 3> enchantingClueLevels{};
    std::int32_t enchantingBookshelfPower = 0;
    // The player's enchantment seed. Presentation only: the screen seeds its
    // Standard-Galactic gibberish name generator from it, exactly as vanilla's
    // EnchantmentNames#initSeed does with EnchantmentMenu's own seed DataSlot.
    std::int32_t enchantingSeed = 0;
    // ENCH-3: the anvil screen's display state — the two inputs, the derived
    // result and its price. `anvilCost` is shown even when the result is empty:
    // that is exactly the "Too Expensive!" case, where vanilla shows the number
    // and withholds the item.
    ItemStack anvilLeft{};
    ItemStack anvilRight{};
    ItemStack anvilResult{};
    std::int32_t anvilCost = 0;
    // AR-M6: the trade screen's display state — everything a merchant screen
    // draws, and nothing it has to derive.
    //
    // The three slots first (payments and the DERIVED result), then one row per
    // offer. `tradeOfferCount` says how many rows are real; the arrays are
    // fixed so the snapshot stays a POD and the wire stays fixed-width.
    //
    // A row carries `wantsA`/`wantsB`/`gives` (wantsB is empty for every offer
    // in this build, and is here because vanilla's MerchantOffer has costB and
    // the screen draws two payment slots regardless), the level that unlocks
    // it, the uses spent and the ceiling, and two flags the screen would
    // otherwise have to work out for itself: locked (drawn greyed) and out of
    // stock (drawn with the out-of-stock sprite).
    ItemStack tradePaymentA{};
    ItemStack tradePaymentB{};
    ItemStack tradeResult{};
    std::array<ItemStack, kSnapshotTradeOffers> tradeWantsA{};
    std::array<ItemStack, kSnapshotTradeOffers> tradeWantsB{};
    std::array<ItemStack, kSnapshotTradeOffers> tradeGives{};
    std::array<std::uint8_t, kSnapshotTradeOffers> tradeOfferLevels{};
    std::array<std::uint8_t, kSnapshotTradeOffers> tradeOfferUses{};
    std::array<std::uint8_t, kSnapshotTradeOffers> tradeOfferMaxUses{};
    std::array<std::uint8_t, kSnapshotTradeOffers> tradeOfferLocked{};
    std::array<std::uint8_t, kSnapshotTradeOffers> tradeOfferOutOfStock{};
    std::uint8_t tradeOfferCount = 0U;
    // Which row the player picked; `kNoSelectedTradeOffer` when none is.
    std::uint8_t tradeSelectedOffer = kNoSelectedTradeOffer;
    // The villager's level and the level bar's two numbers. A
    // `tradeXpForNextLevel` of 0 means the villager is at the ceiling and the
    // bar is not drawn at all.
    std::uint8_t tradeVillagerLevel = 1U;
    std::int32_t tradeXpInLevel = 0;
    std::int32_t tradeXpForNextLevel = 0;
};

} // namespace mc::gameplay
