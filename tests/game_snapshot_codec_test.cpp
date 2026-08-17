// N4 message-boundary slice 2: the per-tick snapshots (server→client mirror)
// survive a byte round trip, split back out of a stream at their frame
// boundary, and skip an unknown tag by size — the same forward-compatibility as
// the command codec, so a future transport carries intents and mirrors in one
// stream with one framing.

#include "gameplay/GameSnapshotCodec.hpp"

#include "gameplay/Item.hpp"
#include "persistence/SaveStream.hpp"
#include "world/Block.hpp"

#include <glm/vec3.hpp>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

using namespace mc;

namespace {

void checkRoundTrip(const gameplay::PublishedSnapshot& snapshot) {
    const auto bytes = gameplay::encodeSnapshot(snapshot);
    const auto decoded = gameplay::decodeSnapshot(bytes);
    assert(decoded.has_value());
    assert(snapshot == *decoded);
}

// A fully-populated player snapshot, so every field is exercised.
[[nodiscard]] gameplay::PlayerTickSnapshot populatedPlayer() {
    gameplay::PlayerTickSnapshot snap;
    snap.serverTick = 123'456U;
    snap.swing.active = true;
    snap.swing.hand = gameplay::InteractionHand::Main;
    snap.swing.animation = gameplay::SwingAnimation::Break;
    snap.swing.sequence = 7U;
    snap.swing.startedTick = 120'000U;
    snap.swing.durationTicks = 6U;
    snap.swing.elapsedTicks = 4U;
    snap.swing.previousProgress = 0.5F;
    snap.swing.progress = 0.75F;
    snap.use.active = true;
    snap.use.hand = gameplay::InteractionHand::Off;
    snap.use.animation = gameplay::UseAnimation::Eat;
    snap.use.startedTick = 121'000U;
    snap.use.durationTicks = 32U;
    snap.use.remainingTicks = 20U;
    snap.use.previousRemainingTicks = 21U;
    snap.physicsPrevious = {10.5F, 64.25F, -8.0F};
    snap.physicsCurrent = {10.75F, 64.1F, -7.5F};
    snap.previousStride = 1.0F;
    snap.stride = 1.5F;
    snap.previousSpeed = 0.4F;
    snap.speed = 0.6F;
    snap.sneaking = true;
    snap.flying = false;
    snap.sprinting = true;
    snap.inWater = false;
    snap.onGround = true;
    snap.previousFieldOfViewMultiplier = 1.0F;
    snap.fieldOfViewMultiplier = 1.1F;
    snap.heldStack = {world::Block::Air, 3U, &gameplay::items::Diamond};
    snap.health = 17.5F;
    snap.foodLevel = 14;
    snap.airTicks = 250;
    snap.ticksSinceDamage = 40;
    snap.gameMode = gameplay::GameMode::Creative;
    snap.eating = true;
    snap.selectedHotbarSlot = 5U;
    return snap;
}

// A fully-populated world snapshot with chests, container display and a block
// stack in the inventory, so the arrays and vectors are exercised.
[[nodiscard]] gameplay::WorldSnapshot populatedWorld() {
    gameplay::WorldSnapshot snap;
    snap.serverTick = 99'000U;
    snap.previousRainGradient = 0.2F;
    snap.rainGradient = 0.5F;
    snap.previousThunderGradient = 0.0F;
    snap.thunderGradient = 0.25F;
    snap.raining = true;
    snap.thundering = true;
    snap.dayTimeTicks = 18'000.0;
    snap.clocks[static_cast<std::size_t>(world::ClockId::Overworld)] =
        world::ClockState{18'000U, 0.5F, 1.0F, false};
    snap.doDaylightCycle = true;
    snap.doWeatherCycle = false;
    snap.worldSpawnPosition = {24.0F, 76.38F, 24.0F};
    snap.playerSpawnPosition = {12.0F, 65.0F, 30.0F};
    snap.playerSpawnYaw = 90.0F;
    snap.hasPlayerSpawn = true;
    snap.chests = {gameplay::WorldSnapshot::ChestRenderState{
        {4, 64, 8}, 0.1F, 0.6F}};
    snap.inventorySlots[0] = {world::Block::Stone, 1U, gameplay::blockItemFor(world::Block::Stone)};
    snap.cursorStack = {world::Block::Air, 2U, &gameplay::items::Apple};
    snap.chestItems[1] = {world::Block::DiamondOre, 5U, nullptr};
    snap.tableCraftingGrid[2] = {world::Block::Air, 1U, &gameplay::items::Stick};
    snap.tableCraftingOutput = {world::Block::Air, 1U, &gameplay::items::Diamond};
    snap.playerCraftingGrid[3] = {world::Block::OakLog, 1U, nullptr};
    snap.playerCraftingOutput = {world::Block::OakPlanks, 1U,
                                 gameplay::blockItemFor(world::Block::OakPlanks)};
    snap.furnaceInput = {world::Block::IronOre, 1U, nullptr};
    snap.furnaceFuel = {world::Block::Air, 1U, &gameplay::items::Coal};
    snap.furnaceOutput = {world::Block::Air, 1U, &gameplay::items::IronIngot};
    snap.furnaceFuelProgress = 0.3F;
    snap.furnaceCookProgress = 0.6F;
    return snap;
}

}  // namespace

int main() {
    // --- A fully-populated player snapshot round-trips with every field. ---
    checkRoundTrip(gameplay::PlayerTickSnapshot{populatedPlayer()});

    // --- A fully-populated world snapshot (chests + container display + block
    // and item stacks) round-trips. ---
    checkRoundTrip(gameplay::WorldSnapshot{populatedWorld()});

    // --- A default snapshot also round-trips (all-empty arrays, no chests). ---
    checkRoundTrip(gameplay::WorldSnapshot{});

    // --- Two snapshots in one stream split back at their frame boundaries. ---
    {
        const gameplay::PublishedSnapshot first{gameplay::PlayerTickSnapshot{populatedPlayer()}};
        const gameplay::PublishedSnapshot second{gameplay::WorldSnapshot{populatedWorld()}};
        auto stream = gameplay::encodeSnapshot(first);
        const auto secondBytes = gameplay::encodeSnapshot(second);
        const auto firstSize = stream.size();
        stream.insert(stream.end(), secondBytes.begin(), secondBytes.end());

        const auto boundary = gameplay::encodedSnapshotSize(stream);
        assert(boundary == firstSize);
        const auto decodedFirst =
            gameplay::decodeSnapshot(std::span{stream.data(), boundary});
        assert(decodedFirst.has_value() && first == *decodedFirst);
        const auto decodedSecond = gameplay::decodeSnapshot(
            std::span{stream.data() + boundary, stream.size() - boundary});
        assert(decodedSecond.has_value() && second == *decodedSecond);
    }

    // --- An unknown snapshot tag (a newer build's mirror) is skipped by size,
    // not fatal. ---
    {
        std::vector<std::uint8_t> bytes;
        bytes.push_back(99U);
        persistence::appendInteger(bytes, std::uint32_t{3});
        bytes.push_back(1);
        bytes.push_back(2);
        bytes.push_back(3);
        assert(gameplay::encodedSnapshotSize(bytes) == 5U + 3U);
        assert(!gameplay::decodeSnapshot(bytes).has_value());
    }

    // --- A truncated frame reports an incomplete size and refuses to decode. ---
    {
        std::vector<std::uint8_t> bytes;
        bytes.push_back(13U);
        persistence::appendInteger(bytes, std::uint32_t{1000U});
        bytes.push_back(1);
        assert(gameplay::encodedSnapshotSize(bytes) == 0U);
        assert(!gameplay::decodeSnapshot(bytes).has_value());
    }

    std::cout << "PASS: game_snapshot_codec_test\n";
    return 0;
}
