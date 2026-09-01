#include "gameplay/GameSnapshotCodec.hpp"

#include "gameplay/GameCommandCodec.hpp"
#include "gameplay/StreamCodec.hpp"

#include <cstdint>
#include <stdexcept>

namespace mc::gameplay {
namespace {

// Snapshot tags, kept separate from the command tags so a mixed stream never
// routes a snapshot to the command decoder or vice versa. Derived from the
// shared layout in GameCommandCodec.hpp rather than written as literals — see
// its banner for why (a literal here silently collided with a command tag the
// moment the command variant grew).
constexpr std::uint8_t kPlayerTickTag = kSnapshotTagBase;
constexpr std::uint8_t kWorldTag = static_cast<std::uint8_t>(kSnapshotTagBase + 1U);
// After the event tags, so the entity snapshot rides the same mixed stream
// without colliding — see NetMessage's tag ranges.

void appendBool(std::vector<std::uint8_t>& bytes, bool value) {
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(value ? 1 : 0));
}
[[nodiscard]] bool readBool(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    return persistence::readInteger<std::uint8_t>(bytes, cursor) != 0;
}

void appendSwing(std::vector<std::uint8_t>& bytes, const SwingState& swing) {
    appendBool(bytes, swing.active);
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(swing.hand));
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(swing.animation));
    persistence::appendInteger(bytes, swing.sequence);
    persistence::appendInteger(bytes, swing.startedTick);
    persistence::appendInteger(bytes, swing.durationTicks);
    persistence::appendInteger(bytes, swing.elapsedTicks);
    persistence::appendFloat(bytes, swing.previousProgress);
    persistence::appendFloat(bytes, swing.progress);
}
[[nodiscard]] SwingState readSwing(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    SwingState swing;
    swing.active = readBool(bytes, cursor);
    swing.hand = static_cast<InteractionHand>(persistence::readInteger<std::uint8_t>(bytes, cursor));
    swing.animation = static_cast<SwingAnimation>(
        persistence::readInteger<std::uint8_t>(bytes, cursor));
    swing.sequence = persistence::readInteger<std::uint64_t>(bytes, cursor);
    swing.startedTick = persistence::readInteger<std::uint64_t>(bytes, cursor);
    swing.durationTicks = persistence::readInteger<std::uint32_t>(bytes, cursor);
    swing.elapsedTicks = persistence::readInteger<std::uint32_t>(bytes, cursor);
    swing.previousProgress = persistence::readFloat(bytes, cursor);
    swing.progress = persistence::readFloat(bytes, cursor);
    return swing;
}

void appendUse(std::vector<std::uint8_t>& bytes, const ItemUseState& use) {
    appendBool(bytes, use.active);
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(use.hand));
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(use.animation));
    persistence::appendInteger(bytes, use.startedTick);
    persistence::appendInteger(bytes, use.durationTicks);
    persistence::appendInteger(bytes, use.remainingTicks);
    persistence::appendInteger(bytes, use.previousRemainingTicks);
}
[[nodiscard]] ItemUseState readUse(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
    ItemUseState use;
    use.active = readBool(bytes, cursor);
    use.hand = static_cast<InteractionHand>(persistence::readInteger<std::uint8_t>(bytes, cursor));
    use.animation =
        static_cast<UseAnimation>(persistence::readInteger<std::uint8_t>(bytes, cursor));
    use.startedTick = persistence::readInteger<std::uint64_t>(bytes, cursor);
    use.durationTicks = persistence::readInteger<std::uint32_t>(bytes, cursor);
    use.remainingTicks = persistence::readInteger<std::uint32_t>(bytes, cursor);
    use.previousRemainingTicks = persistence::readInteger<std::uint32_t>(bytes, cursor);
    return use;
}

void appendPlayerTick(std::vector<std::uint8_t>& bytes, const PlayerTickSnapshot& snap) {
    persistence::appendInteger(bytes, snap.serverTick);
    appendSwing(bytes, snap.swing);
    appendUse(bytes, snap.use);
    codec::appendVec3(bytes, snap.physicsPrevious);
    codec::appendVec3(bytes, snap.physicsCurrent);
    persistence::appendFloat(bytes, snap.previousStride);
    persistence::appendFloat(bytes, snap.stride);
    persistence::appendFloat(bytes, snap.previousSpeed);
    persistence::appendFloat(bytes, snap.speed);
    persistence::appendFloat(bytes, snap.previousWalkAmount);
    persistence::appendFloat(bytes, snap.walkAmount);
    persistence::appendFloat(bytes, snap.previousWalkPosition);
    persistence::appendFloat(bytes, snap.walkPosition);
    appendBool(bytes, snap.sneaking);
    appendBool(bytes, snap.flying);
    appendBool(bytes, snap.sprinting);
    appendBool(bytes, snap.inWater);
    appendBool(bytes, snap.onGround);
    persistence::appendFloat(bytes, snap.previousFieldOfViewMultiplier);
    persistence::appendFloat(bytes, snap.fieldOfViewMultiplier);
    codec::appendItemStack(bytes, snap.heldStack);
    // The dig in progress (crack overlay): active, the cell, and the tick it
    // started. Without it a mirror reads an inactive dig and mining shows no
    // crack stages.
    appendBool(bytes, snap.digging.active);
    codec::appendIvec3(bytes, snap.digging.target);
    persistence::appendInteger(bytes, snap.digging.startedTick);
    persistence::appendFloat(bytes, snap.health);
    persistence::appendInteger(bytes, static_cast<std::int32_t>(snap.foodLevel));
    persistence::appendInteger(bytes, static_cast<std::int32_t>(snap.airTicks));
    persistence::appendInteger(bytes, static_cast<std::int32_t>(snap.ticksSinceDamage));
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(snap.gameMode));
    appendBool(bytes, snap.eating);
    persistence::appendInteger(bytes, static_cast<std::uint64_t>(snap.selectedHotbarSlot));
    // XP-0: the HUD's experience bar fill and level number.
    persistence::appendInteger(bytes, static_cast<std::int32_t>(snap.experienceLevel));
    persistence::appendFloat(bytes, snap.experienceProgress);
}

[[nodiscard]] PlayerTickSnapshot readPlayerTick(std::span<const std::uint8_t> bytes,
                                                std::size_t& cursor) {
    PlayerTickSnapshot snap;
    snap.serverTick = persistence::readInteger<std::uint64_t>(bytes, cursor);
    snap.swing = readSwing(bytes, cursor);
    snap.use = readUse(bytes, cursor);
    snap.physicsPrevious = codec::readVec3(bytes, cursor);
    snap.physicsCurrent = codec::readVec3(bytes, cursor);
    snap.previousStride = persistence::readFloat(bytes, cursor);
    snap.stride = persistence::readFloat(bytes, cursor);
    snap.previousSpeed = persistence::readFloat(bytes, cursor);
    snap.speed = persistence::readFloat(bytes, cursor);
    snap.previousWalkAmount = persistence::readFloat(bytes, cursor);
    snap.walkAmount = persistence::readFloat(bytes, cursor);
    snap.previousWalkPosition = persistence::readFloat(bytes, cursor);
    snap.walkPosition = persistence::readFloat(bytes, cursor);
    snap.sneaking = readBool(bytes, cursor);
    snap.flying = readBool(bytes, cursor);
    snap.sprinting = readBool(bytes, cursor);
    snap.inWater = readBool(bytes, cursor);
    snap.onGround = readBool(bytes, cursor);
    snap.previousFieldOfViewMultiplier = persistence::readFloat(bytes, cursor);
    snap.fieldOfViewMultiplier = persistence::readFloat(bytes, cursor);
    const auto held = codec::readItemStack(bytes, cursor);
    if (!held.has_value()) {
        throw std::runtime_error("Snapshot holds an unknown item");
    }
    snap.heldStack = *held;
    snap.digging.active = readBool(bytes, cursor);
    snap.digging.target = codec::readIvec3(bytes, cursor);
    snap.digging.startedTick = persistence::readInteger<std::uint64_t>(bytes, cursor);
    snap.health = persistence::readFloat(bytes, cursor);
    snap.foodLevel = persistence::readInteger<std::int32_t>(bytes, cursor);
    snap.airTicks = persistence::readInteger<std::int32_t>(bytes, cursor);
    snap.ticksSinceDamage = persistence::readInteger<std::int32_t>(bytes, cursor);
    snap.gameMode = static_cast<GameMode>(persistence::readInteger<std::uint8_t>(bytes, cursor));
    snap.eating = readBool(bytes, cursor);
    snap.selectedHotbarSlot =
        static_cast<std::size_t>(persistence::readInteger<std::uint64_t>(bytes, cursor));
    snap.experienceLevel = persistence::readInteger<std::int32_t>(bytes, cursor);
    snap.experienceProgress = persistence::readFloat(bytes, cursor);
    return snap;
}

void appendWorld(std::vector<std::uint8_t>& bytes, const WorldSnapshot& snap) {
    persistence::appendInteger(bytes, snap.serverTick);
    persistence::appendFloat(bytes, snap.previousRainGradient);
    persistence::appendFloat(bytes, snap.rainGradient);
    persistence::appendFloat(bytes, snap.previousThunderGradient);
    persistence::appendFloat(bytes, snap.thunderGradient);
    appendBool(bytes, snap.raining);
    appendBool(bytes, snap.thundering);
    persistence::appendDouble(bytes, snap.dayTimeTicks);
    for (const auto& clock : snap.clocks) {
        persistence::appendInteger(bytes, clock.totalTicks);
        persistence::appendFloat(bytes, clock.partialTick);
        persistence::appendFloat(bytes, clock.rate);
        appendBool(bytes, clock.paused);
    }
    appendBool(bytes, snap.doDaylightCycle);
    appendBool(bytes, snap.doWeatherCycle);
    codec::appendVec3(bytes, snap.worldSpawnPosition);
    codec::appendVec3(bytes, snap.playerSpawnPosition);
    persistence::appendFloat(bytes, snap.playerSpawnYaw);
    appendBool(bytes, snap.hasPlayerSpawn);
    // The open container binding: which screen is open and, for a block-backed
    // container, the chest/furnace cell. The HUD dispatches the container it
    // draws from this, so it must ride the wire — without it a mirror always
    // reads the default PlayerInventory and every container opens as the pack.
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(snap.openContainerScreen));
    appendBool(bytes, snap.openChest.has_value());
    if (snap.openChest.has_value()) {
        persistence::appendInteger(bytes, static_cast<std::int32_t>(snap.openChest->x));
        persistence::appendInteger(bytes, static_cast<std::int32_t>(snap.openChest->y));
        persistence::appendInteger(bytes, static_cast<std::int32_t>(snap.openChest->z));
    }
    appendBool(bytes, snap.openFurnace.has_value());
    if (snap.openFurnace.has_value()) {
        codec::appendIvec3(bytes, *snap.openFurnace);
    }
    persistence::appendInteger(bytes, static_cast<std::uint32_t>(snap.chests.size()));
    for (const auto& chest : snap.chests) {
        persistence::appendInteger(bytes, static_cast<std::int32_t>(chest.position.x));
        persistence::appendInteger(bytes, static_cast<std::int32_t>(chest.position.y));
        persistence::appendInteger(bytes, static_cast<std::int32_t>(chest.position.z));
        persistence::appendFloat(bytes, chest.previousLidAngle);
        persistence::appendFloat(bytes, chest.lidAngle);
    }
    for (const auto& stack : snap.inventorySlots) {
        codec::appendItemStack(bytes, stack);
    }
    codec::appendItemStack(bytes, snap.cursorStack);
    for (const auto& stack : snap.chestItems) {
        codec::appendItemStack(bytes, stack);
    }
    for (const auto& stack : snap.tableCraftingGrid) {
        codec::appendItemStack(bytes, stack);
    }
    codec::appendItemStack(bytes, snap.tableCraftingOutput);
    for (const auto& stack : snap.playerCraftingGrid) {
        codec::appendItemStack(bytes, stack);
    }
    codec::appendItemStack(bytes, snap.playerCraftingOutput);
    codec::appendItemStack(bytes, snap.furnaceInput);
    codec::appendItemStack(bytes, snap.furnaceFuel);
    codec::appendItemStack(bytes, snap.furnaceOutput);
    persistence::appendFloat(bytes, snap.furnaceFuelProgress);
    persistence::appendFloat(bytes, snap.furnaceCookProgress);
    // EQ-0: the equipment slots, live wire format like every other ItemStack
    // array here — no version gate needed (see StreamCodec.hpp's appendItemStack
    // banner: this message shape is never persisted across builds).
    for (const auto& stack : snap.equipmentSlots) {
        codec::appendItemStack(bytes, stack);
    }
    // ENCH-2: the enchanting screen's display state. Same live-wire rule as the
    // arrays above — this message is never persisted, so no version gate.
    codec::appendItemStack(bytes, snap.enchantingItem);
    codec::appendItemStack(bytes, snap.enchantingLapis);
    for (std::size_t slot = 0; slot < snap.enchantingRequiredLevels.size(); ++slot) {
        persistence::appendInteger(bytes, snap.enchantingRequiredLevels[slot]);
        persistence::appendInteger(bytes, snap.enchantingClueIds[slot]);
        persistence::appendInteger(bytes, snap.enchantingClueLevels[slot]);
    }
    persistence::appendInteger(bytes, snap.enchantingBookshelfPower);
    persistence::appendInteger(bytes, snap.enchantingSeed);
}

[[nodiscard]] std::optional<WorldSnapshot> readWorld(std::span<const std::uint8_t> bytes,
                                                     std::size_t& cursor) {
    WorldSnapshot snap;
    snap.serverTick = persistence::readInteger<std::uint64_t>(bytes, cursor);
    snap.previousRainGradient = persistence::readFloat(bytes, cursor);
    snap.rainGradient = persistence::readFloat(bytes, cursor);
    snap.previousThunderGradient = persistence::readFloat(bytes, cursor);
    snap.thunderGradient = persistence::readFloat(bytes, cursor);
    snap.raining = readBool(bytes, cursor);
    snap.thundering = readBool(bytes, cursor);
    snap.dayTimeTicks = persistence::readDouble(bytes, cursor);
    for (auto& clock : snap.clocks) {
        clock.totalTicks = persistence::readInteger<std::uint64_t>(bytes, cursor);
        clock.partialTick = persistence::readFloat(bytes, cursor);
        clock.rate = persistence::readFloat(bytes, cursor);
        clock.paused = readBool(bytes, cursor);
    }
    snap.doDaylightCycle = readBool(bytes, cursor);
    snap.doWeatherCycle = readBool(bytes, cursor);
    snap.worldSpawnPosition = codec::readVec3(bytes, cursor);
    snap.playerSpawnPosition = codec::readVec3(bytes, cursor);
    snap.playerSpawnYaw = persistence::readFloat(bytes, cursor);
    snap.hasPlayerSpawn = readBool(bytes, cursor);
    snap.openContainerScreen =
        static_cast<ContainerScreen>(persistence::readInteger<std::uint8_t>(bytes, cursor));
    if (readBool(bytes, cursor)) {
        ChestPosition chest;
        chest.x = persistence::readInteger<std::int32_t>(bytes, cursor);
        chest.y = persistence::readInteger<std::int32_t>(bytes, cursor);
        chest.z = persistence::readInteger<std::int32_t>(bytes, cursor);
        snap.openChest = chest;
    }
    if (readBool(bytes, cursor)) {
        snap.openFurnace = codec::readIvec3(bytes, cursor);
    }
    const auto chestCount = persistence::readInteger<std::uint32_t>(bytes, cursor);
    snap.chests.reserve(chestCount);
    for (std::uint32_t index = 0; index < chestCount; ++index) {
        WorldSnapshot::ChestRenderState chest;
        chest.position.x = persistence::readInteger<std::int32_t>(bytes, cursor);
        chest.position.y = persistence::readInteger<std::int32_t>(bytes, cursor);
        chest.position.z = persistence::readInteger<std::int32_t>(bytes, cursor);
        chest.previousLidAngle = persistence::readFloat(bytes, cursor);
        chest.lidAngle = persistence::readFloat(bytes, cursor);
        snap.chests.push_back(chest);
    }
    const auto readStack = [&](ItemStack& out) {
        const auto stack = codec::readItemStack(bytes, cursor);
        if (!stack.has_value()) {
            return false;
        }
        out = *stack;
        return true;
    };
    for (auto& stack : snap.inventorySlots) {
        if (!readStack(stack)) return std::nullopt;
    }
    if (!readStack(snap.cursorStack)) return std::nullopt;
    for (auto& stack : snap.chestItems) {
        if (!readStack(stack)) return std::nullopt;
    }
    for (auto& stack : snap.tableCraftingGrid) {
        if (!readStack(stack)) return std::nullopt;
    }
    if (!readStack(snap.tableCraftingOutput)) return std::nullopt;
    for (auto& stack : snap.playerCraftingGrid) {
        if (!readStack(stack)) return std::nullopt;
    }
    if (!readStack(snap.playerCraftingOutput)) return std::nullopt;
    if (!readStack(snap.furnaceInput)) return std::nullopt;
    if (!readStack(snap.furnaceFuel)) return std::nullopt;
    if (!readStack(snap.furnaceOutput)) return std::nullopt;
    snap.furnaceFuelProgress = persistence::readFloat(bytes, cursor);
    snap.furnaceCookProgress = persistence::readFloat(bytes, cursor);
    for (auto& stack : snap.equipmentSlots) {
        if (!readStack(stack)) return std::nullopt;
    }
    if (!readStack(snap.enchantingItem)) return std::nullopt;
    if (!readStack(snap.enchantingLapis)) return std::nullopt;
    for (std::size_t slot = 0; slot < snap.enchantingRequiredLevels.size(); ++slot) {
        snap.enchantingRequiredLevels[slot] = persistence::readInteger<std::int32_t>(bytes, cursor);
        snap.enchantingClueIds[slot] = persistence::readInteger<std::uint8_t>(bytes, cursor);
        snap.enchantingClueLevels[slot] = persistence::readInteger<std::uint8_t>(bytes, cursor);
    }
    snap.enchantingBookshelfPower = persistence::readInteger<std::int32_t>(bytes, cursor);
    snap.enchantingSeed = persistence::readInteger<std::int32_t>(bytes, cursor);
    return snap;
}

// --- Entity render snapshot: creatures, drops, falling blocks ---
// Only the render-relevant fields ride the wire (not the sim-only velocity of a
// drop or a falling block's vertical velocity / removed flag), keeping the
// per-tick entity payload — the largest, one record per entity — lean.

void appendCreature(std::vector<std::uint8_t>& bytes, const EntityRenderState& s) {
    codec::appendEntityType(bytes, s.type);
    persistence::appendInteger(bytes, s.id);
    codec::appendVec3(bytes, s.position);
    codec::appendVec3(bytes, s.previousPosition);
    persistence::appendFloat(bytes, s.yaw);
    persistence::appendFloat(bytes, s.previousYaw);
    persistence::appendFloat(bytes, s.walkDistance);
    persistence::appendFloat(bytes, s.previousWalkDistance);
    persistence::appendInteger(bytes, static_cast<std::int32_t>(s.hurtTicks));
    persistence::appendInteger(bytes, static_cast<std::int32_t>(s.deathTicks));
    persistence::appendFloat(bytes, s.scale);
    persistence::appendInteger(bytes, dyeColorId(s.color));
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(s.sheared ? 1 : 0));
}

void appendDrop(std::vector<std::uint8_t>& bytes, const ItemEntity& drop) {
    codec::appendVec3(bytes, drop.position);
    codec::appendVec3(bytes, drop.previousPosition);
    codec::appendItemStack(bytes, drop.stack);
    persistence::appendInteger(bytes, static_cast<std::uint32_t>(drop.ageTicks));
    persistence::appendFloat(bytes, drop.visualPhase);
}

void appendFalling(std::vector<std::uint8_t>& bytes, const FallingBlockEntity& block) {
    codec::appendVec3(bytes, block.position);
    codec::appendVec3(bytes, block.previousPosition);
    codec::appendBlock(bytes, block.block);
}

// XP-1: the orb's denomination and stacked count go over the wire too — a
// value-dependent icon (getIcon's tier table) is a render-side decision, but
// it needs both fields to make it, and pickupDelay/age are simulation-only
// (never read by the draw pass), so they are deliberately left off, matching
// the drop record's own choice to carry ageTicks but not the merge-only state.
void appendExperienceOrb(std::vector<std::uint8_t>& bytes, const ExperienceOrb& orb) {
    codec::appendVec3(bytes, orb.position);
    codec::appendVec3(bytes, orb.previousPosition);
    persistence::appendInteger(bytes, orb.value);
    persistence::appendInteger(bytes, orb.count);
}

// RW-0: the flying/stuck projectile's render-relevant fields. previousPosition
// rides along (unlike the orb record above, which only needs position for the
// draw pass) because RW-0's own card asks for interpolation the same way
// items/orbs get it; inGround/critical pick the model's resting-vs-flying pose
// once PX reads this. shooterId/pickupItem/lifeTicks are simulation-only
// (never read by the draw pass), the same "leave the sim-only fields off the
// wire" choice appendExperienceOrb already makes for pickupDelay/age.
void appendProjectile(std::vector<std::uint8_t>& bytes, const Projectile& projectile) {
    codec::appendVec3(bytes, projectile.position);
    codec::appendVec3(bytes, projectile.previousPosition);
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(projectile.critical ? 1U : 0U));
    persistence::appendInteger(bytes, static_cast<std::uint8_t>(projectile.inGround ? 1U : 0U));
}

void appendEntities(std::vector<std::uint8_t>& bytes, const EntityRenderSnapshot& snap) {
    persistence::appendInteger(bytes, static_cast<std::uint32_t>(snap.entities().size()));
    for (const auto& creature : snap.entities()) {
        appendCreature(bytes, creature);
    }
    persistence::appendInteger(bytes, static_cast<std::uint32_t>(snap.items().size()));
    for (const auto& drop : snap.items()) {
        appendDrop(bytes, drop);
    }
    persistence::appendInteger(bytes, static_cast<std::uint32_t>(snap.experienceOrbs().size()));
    for (const auto& orb : snap.experienceOrbs()) {
        appendExperienceOrb(bytes, orb);
    }
    persistence::appendInteger(bytes, static_cast<std::uint32_t>(snap.projectiles().size()));
    for (const auto& projectile : snap.projectiles()) {
        appendProjectile(bytes, projectile);
    }
    persistence::appendInteger(bytes, static_cast<std::uint32_t>(snap.fallingBlocks().size()));
    for (const auto& block : snap.fallingBlocks()) {
        appendFalling(bytes, block);
    }
}

[[nodiscard]] EntityRenderSnapshot readEntities(std::span<const std::uint8_t> bytes,
                                                std::size_t& cursor,
                                                const BlockIdRemap* remap) {
    std::vector<EntityRenderState> creatures;
    const auto creatureCount = persistence::readInteger<std::uint32_t>(bytes, cursor);
    creatures.reserve(creatureCount);
    for (std::uint32_t index = 0; index < creatureCount; ++index) {
        EntityRenderState s;
        s.type = codec::readEntityType(bytes, cursor);
        s.id = persistence::readInteger<std::uint64_t>(bytes, cursor);
        s.position = codec::readVec3(bytes, cursor);
        s.previousPosition = codec::readVec3(bytes, cursor);
        s.yaw = persistence::readFloat(bytes, cursor);
        s.previousYaw = persistence::readFloat(bytes, cursor);
        s.walkDistance = persistence::readFloat(bytes, cursor);
        s.previousWalkDistance = persistence::readFloat(bytes, cursor);
        s.hurtTicks = persistence::readInteger<std::int32_t>(bytes, cursor);
        s.deathTicks = persistence::readInteger<std::int32_t>(bytes, cursor);
        s.scale = persistence::readFloat(bytes, cursor);
        s.color = dyeColorFromId(persistence::readInteger<std::uint8_t>(bytes, cursor));
        s.sheared = persistence::readInteger<std::uint8_t>(bytes, cursor) != 0U;
        // A creature of a species this build does not know is skipped, not drawn.
        if (s.type != nullptr) {
            creatures.push_back(s);
        }
    }
    std::vector<ItemEntity> drops;
    const auto dropCount = persistence::readInteger<std::uint32_t>(bytes, cursor);
    drops.reserve(dropCount);
    for (std::uint32_t index = 0; index < dropCount; ++index) {
        ItemEntity drop;
        drop.position = codec::readVec3(bytes, cursor);
        drop.previousPosition = codec::readVec3(bytes, cursor);
        const auto stack = codec::readItemStack(bytes, cursor);
        drop.ageTicks = persistence::readInteger<std::uint32_t>(bytes, cursor);
        drop.visualPhase = persistence::readFloat(bytes, cursor);
        // An unknown item is skipped rather than drawn as air.
        if (stack.has_value()) {
            drop.stack = *stack;
            drops.push_back(drop);
        }
    }
    std::vector<ExperienceOrb> orbs;
    const auto orbCount = persistence::readInteger<std::uint32_t>(bytes, cursor);
    orbs.reserve(orbCount);
    for (std::uint32_t index = 0; index < orbCount; ++index) {
        ExperienceOrb orb;
        orb.position = codec::readVec3(bytes, cursor);
        orb.previousPosition = codec::readVec3(bytes, cursor);
        orb.value = persistence::readInteger<std::int32_t>(bytes, cursor);
        orb.count = persistence::readInteger<std::int32_t>(bytes, cursor);
        // A malformed/zeroed record would draw as an invisible entity; skip it
        // the same way an unknown item or block is skipped above.
        if (orb.value > 0 && orb.count > 0) {
            orbs.push_back(orb);
        }
    }
    std::vector<Projectile> projectiles;
    const auto projectileCount = persistence::readInteger<std::uint32_t>(bytes, cursor);
    projectiles.reserve(projectileCount);
    for (std::uint32_t index = 0; index < projectileCount; ++index) {
        Projectile projectile;
        projectile.position = codec::readVec3(bytes, cursor);
        projectile.previousPosition = codec::readVec3(bytes, cursor);
        projectile.critical = persistence::readInteger<std::uint8_t>(bytes, cursor) != 0U;
        projectile.inGround = persistence::readInteger<std::uint8_t>(bytes, cursor) != 0U;
        projectiles.push_back(projectile);
    }
    std::vector<FallingBlockEntity> falling;
    const auto fallingCount = persistence::readInteger<std::uint32_t>(bytes, cursor);
    falling.reserve(fallingCount);
    for (std::uint32_t index = 0; index < fallingCount; ++index) {
        FallingBlockEntity block;
        block.position = codec::readVec3(bytes, cursor);
        block.previousPosition = codec::readVec3(bytes, cursor);
        const auto kind = codec::readBlock(bytes, cursor, remap);
        if (kind.has_value()) {
            block.block = *kind;
            falling.push_back(block);
        }
    }
    EntityRenderSnapshot snapshot;
    snapshot.assign(std::move(creatures), std::move(drops), std::move(orbs),
                    std::move(projectiles), std::move(falling));
    return snapshot;
}

}  // namespace

std::vector<std::uint8_t> encodeSnapshot(const PublishedSnapshot& snapshot) {
    std::vector<std::uint8_t> bytes;
    std::visit(
        [&](const auto& specific) {
            using T = std::decay_t<decltype(specific)>;
            if constexpr (std::is_same_v<T, PlayerTickSnapshot>) {
                codec::appendFrame(bytes, kPlayerTickTag,
                                   [&] { appendPlayerTick(bytes, specific); });
            } else if constexpr (std::is_same_v<T, WorldSnapshot>) {
                codec::appendFrame(bytes, kWorldTag, [&] { appendWorld(bytes, specific); });
            }
        },
        snapshot);
    return bytes;
}

std::size_t encodedSnapshotSize(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < codec::kFrameHeaderBytes) {
        return 0;
    }
    std::size_t cursor = 1;  // skip the tag
    const auto size = persistence::readInteger<std::uint32_t>(bytes, cursor);
    if (bytes.size() < cursor + size) {
        return 0;  // truncated frame: wait for more bytes
    }
    return cursor + size;
}

std::optional<PublishedSnapshot> decodeSnapshot(std::span<const std::uint8_t> bytes) {
    try {
        std::size_t cursor = 0;
        const auto frame = codec::readFrame(bytes, cursor);
        if (!frame.has_value()) {
            return std::nullopt;
        }
        const auto [tag, payloadEnd] = *frame;
        std::optional<PublishedSnapshot> decoded;
        if (tag == kPlayerTickTag) {
            decoded = readPlayerTick(bytes, cursor);
        } else if (tag == kWorldTag) {
            decoded = readWorld(bytes, cursor);
        } else {
            // An unknown snapshot tag (a newer build's mirror): skippable.
            return std::nullopt;
        }
        if (!decoded.has_value() || cursor > payloadEnd) {
            return std::nullopt;
        }
        return decoded;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> encodeEntitySnapshot(const EntityRenderSnapshot& snapshot) {
    std::vector<std::uint8_t> bytes;
    codec::appendFrame(bytes, kEntitySnapshotTag, [&] { appendEntities(bytes, snapshot); });
    return bytes;
}

std::optional<EntityRenderSnapshot> decodeEntitySnapshot(std::span<const std::uint8_t> bytes,
                                                         const BlockIdRemap* remap) {
    try {
        std::size_t cursor = 0;
        const auto frame = codec::readFrame(bytes, cursor);
        if (!frame.has_value()) {
            return std::nullopt;
        }
        const auto [tag, payloadEnd] = *frame;
        if (tag != kEntitySnapshotTag) {
            return std::nullopt;  // unknown tag
        }
        auto decoded = readEntities(bytes, cursor, remap);
        if (cursor > payloadEnd) {
            return std::nullopt;
        }
        return decoded;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace mc::gameplay
