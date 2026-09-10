#pragma once

// AR-M5: the villager's own data — profession, level, trade offers.
//
// Deliberately NOT a Brain/Memory layer. 26.1 drives the villager off
// Brain<Villager> with 51 behaviours, 8 memories, 9 sensors and a Schedule, and
// this build's MobBrain is a GoalSelector; the audit that opened this line says
// plainly that the memory layer is a real debt and that it is NOT the villager's
// prerequisite — the POI claim is. So the single-profession closed loop this
// task asks for (composter + farmer + farmland + trading) is carried by the
// fields below, on the shared SimpleEntity, the same way `sheared`, `swell` and
// `loveTicks` already idle at their defaults for every species that is not a
// sheep or a creeper.
//
// The debt is registered, not hidden: every villager field added here is one
// more argument for BRN-1, and the task report says so.

#include "gameplay/Item.hpp"
#include "world/Block.hpp"
#include "world/BlockState.hpp"
#include "world/World.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace mc::gameplay::entities {

// VillagerProfession. Only the two this loop needs exist: an unemployed
// villager, and the farmer it becomes when it claims a composter. The other
// twelve are absent rather than stubbed — a profession with no workstation
// block, no trades and no work behaviour would be exactly the empty shell this
// project's planning rules forbid.
enum class VillagerProfession : std::uint8_t {
    None,
    Farmer,
};

// The workstation block a profession claims. `Air` means "this profession has no
// job site", which is what keeps None out of the claim scan without a special
// case at the call site.
[[nodiscard]] constexpr world::Block professionWorkstation(VillagerProfession profession) {
    switch (profession) {
    case VillagerProfession::Farmer: return world::Block::Composter;
    case VillagerProfession::None: break;
    }
    return world::Block::Air;
}

// The profession a workstation block hands out. The inverse of the above, and
// the whole of "claiming a composter makes you a farmer".
[[nodiscard]] constexpr VillagerProfession professionForWorkstation(world::Block block) {
    if (block == world::Block::Composter) {
        return VillagerProfession::Farmer;
    }
    return VillagerProfession::None;
}

[[nodiscard]] constexpr std::string_view professionName(VillagerProfession profession) {
    switch (profession) {
    case VillagerProfession::Farmer: return "farmer";
    case VillagerProfession::None: break;
    }
    return "none";
}

// How much a farmer carries back to its composter before it stops reaping.
// Vanilla's villager has an eight-slot inventory and empties it wholesale; this
// build has one slot, and this is its stack size.
inline constexpr std::uint8_t kVillagerCarryCapacity = 8U;

// VillagerData.MIN/MAX_VILLAGER_LEVEL and NEXT_LEVEL_XP_THRESHOLDS.
inline constexpr int kVillagerMinLevel = 1;
inline constexpr int kVillagerMaxLevel = 5;
inline constexpr std::array<int, 5> kVillagerLevelXpThresholds{{0, 10, 70, 150, 250}};

// VillagerData#canLevelUp: levels 1..4 can still climb; 5 is the ceiling.
[[nodiscard]] constexpr bool villagerCanLevelUp(int level) {
    return level >= kVillagerMinLevel && level < kVillagerMaxLevel;
}

// The experience at which `level` becomes `level + 1`, or 0 at the ceiling.
[[nodiscard]] constexpr int villagerXpToNextLevel(int level) {
    return villagerCanLevelUp(level)
               ? kVillagerLevelXpThresholds[static_cast<std::size_t>(level)]
               : 0;
}

// One trade. Vanilla's 26.1 offers are datapack entries (`villager_trade/<prof>/
// <level>/<name>.json`) of exactly this shape: what it wants, what it gives,
// how many times, and the trading experience the villager earns for it.
//
// `wants`/`gives` are an ItemStack because half of the farmer's goods are blocks
// wielded as their BlockItem (pumpkin, melon) — the same split the composter's
// chance table has.
struct VillagerOffer final {
    ItemStack wants{};
    ItemStack gives{};
    // The level at which this offer unlocks (1..5).
    int level = 1;
    // VillagerTrade#max_uses: how many times it can be taken before the
    // villager needs to restock.
    int maxUses = 16;
    // The villager's own trading experience for one use.
    int xp = 0;
};

// The farmer's offers, restricted to what this roster has. Transcribed from
// 26.1's data/minecraft/villager_trade/farmer/<level>/*.json with the counts and
// max_uses unchanged.
//
// Levels 4 and 5 are absent, and level 1-3 are missing four entries, because
// every one of them trades an item this build does not have (beetroot,
// pumpkin_pie, cookie, cake, suspicious_stew, golden_carrot,
// glistering_melon_slice). Registered as a deviation rather than substituted:
// inventing a trade vanilla does not have would be worse than being short one.
inline const std::array<VillagerOffer, 7> kFarmerOffers{{
    // farmer/1
    {ItemStack{world::Block::Air, 20U, &items::Wheat},
     ItemStack{world::Block::Air, 1U, &items::Emerald}, 1, 16, 2},
    {ItemStack{world::Block::Air, 22U, &items::Carrot},
     ItemStack{world::Block::Air, 1U, &items::Emerald}, 1, 16, 2},
    {ItemStack{world::Block::Air, 26U, &items::Potato},
     ItemStack{world::Block::Air, 1U, &items::Emerald}, 1, 16, 2},
    // The one level-1 offer that runs the other way. xp 0: vanilla gives the
    // villager no trading experience for selling bread.
    {ItemStack{world::Block::Air, 1U, &items::Emerald},
     ItemStack{world::Block::Air, 6U, &items::Bread}, 1, 16, 0},
    // farmer/2
    {ItemStack{world::Block::Pumpkin, 6U},
     ItemStack{world::Block::Air, 1U, &items::Emerald}, 2, 12, 10},
    {ItemStack{world::Block::Air, 1U, &items::Emerald},
     ItemStack{world::Block::Air, 4U, &items::Apple}, 2, 16, 5},
    // farmer/3
    {ItemStack{world::Block::Melon, 4U},
     ItemStack{world::Block::Air, 1U, &items::Emerald}, 3, 12, 20},
}};

// CropBlock's own maximum age. A crop is ready to reap at it and nowhere below.
inline constexpr int kMatureCropAge = 7;

[[nodiscard]] inline bool isMatureCrop(const world::World& world, glm::ivec3 cell) {
    const auto state = world.state(cell.x, cell.y, cell.z);
    return world::isCrop(state.block()) && state.age() >= kMatureCropAge;
}

// The item a mature crop hands over when a villager reaps it. Deliberately NOT
// the block's loot table: HarvestFarmland takes the produce and puts the field
// straight back, so the seed half of a wheat harvest never enters this — the
// villager is not mining the crop, it is farming it.
[[nodiscard]] inline const Item* cropProduce(world::Block block) {
    switch (block) {
    case world::Block::WheatCrops: return &items::Wheat;
    case world::Block::Carrots: return &items::Carrot;
    case world::Block::Potatoes: return &items::Potato;
    default: return nullptr;
    }
}

// AR-M5: how many offers one villager can hold uses for. Sized to the largest
// profession table this build has (the farmer's seven) with room to spare, so a
// villager's per-offer use counter is a fixed inline array rather than a heap
// vector on every entity.
inline constexpr std::size_t kMaxVillagerOffers = 8U;

// Whether an offer is unlocked at `level` and still has uses left.
[[nodiscard]] constexpr bool offerAvailable(const VillagerOffer& offer, int level,
                                            std::uint8_t uses) {
    return level >= offer.level && static_cast<int>(uses) < offer.maxUses;
}

// MerchantOffer#increaseUses plus Villager#shouldIncreaseLevel: the trade's
// experience goes on the villager, and crossing the threshold raises its level.
// Returns the new (level, xp) pair so the caller writes both at once and cannot
// raise one without the other.
struct VillagerProgress final {
    int level = kVillagerMinLevel;
    int xp = 0;
};

[[nodiscard]] constexpr VillagerProgress villagerAfterTrade(int level, int xp, int offerXp) {
    VillagerProgress progress{level, xp + offerXp};
    // A while, not an if: a single big trade at level 1 can carry a villager
    // past more than one threshold, and vanilla's own loop does the same.
    while (villagerCanLevelUp(progress.level) &&
           progress.xp >= villagerXpToNextLevel(progress.level)) {
        ++progress.level;
    }
    return progress;
}

// The offers a profession has at all (empty for the unemployed).
[[nodiscard]] inline std::span<const VillagerOffer> offersFor(VillagerProfession profession) {
    if (profession == VillagerProfession::Farmer) {
        return kFarmerOffers;
    }
    return {};
}

} // namespace mc::gameplay::entities
