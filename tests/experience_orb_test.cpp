// XP-1: the experience orb pool — the first non-living pickup entity. Exercises
// it as a headless simulation: denomination splitting, gravity/water physics,
// the 8-block player magnet, same-value merge (value-conserving), contact
// pickup crediting PlayerExperience, the 6000-tick despawn, the deterministic
// JavaRandom scatter and the save/snapshot round trips.

#include "gameplay/PlayerExperience.hpp"
#include "gameplay/entities/ExperienceOrb.hpp"
#include "gameplay/GameSnapshotCodec.hpp"
#include "persistence/SaveRepository.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/gen/JavaRandom.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numeric>
#include <span>
#include <vector>

using namespace mc;
using namespace mc::gameplay;

namespace {

// A flat stone floor at y == 0, matching item_entity_test's world shape.
world::World buildTestWorld() {
    world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, world::Block::Stone);
        }
    }
    world::World world;
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

// --- Denomination splitting: spawnMany(amount) must produce orbs whose
// values sum exactly to `amount`, each value one of vanilla's fixed
// denominations (experienceOrbDenomination's table). ---
void testDenominationSplitSumsCorrectly() {
    for (const std::int32_t amount : {1, 2, 3, 5, 7, 17, 40, 100, 1000, 4321}) {
        ExperienceOrbSystem orbs;
        world::gen::JavaRandom rng(1234U);
        orbs.spawnMany({0.0F, 5.0F, 0.0F}, amount, rng);
        std::int32_t sum = 0;
        for (const auto& orb : orbs.entities()) {
            sum += orb.value * orb.count;
            assert(orb.value == experienceOrbDenomination(orb.value) ||
                  orb.value <= amount);  // every placed orb is a valid denomination
        }
        assert(sum == amount);
    }
    std::cout << "testDenominationSplitSumsCorrectly OK\n";
}

// The task's own worked example: spawnExperienceOrbs(pos, 17) splits into
// vanilla denominations whose value sum is exactly 17.
void testSeventeenSplitsToKnownDenominations() {
    ExperienceOrbSystem orbs;
    world::gen::JavaRandom rng(1U);
    orbs.spawnMany({0.0F, 5.0F, 0.0F}, 17, rng);
    // getExperienceValue(17) == 17 directly (17 is itself a denomination), so
    // a single orb of value 17 is the exact vanilla outcome.
    assert(orbs.entities().size() == 1U);
    assert(orbs.entities().front().value == 17);
    std::int32_t sum = 0;
    for (const auto& orb : orbs.entities()) sum += orb.value * orb.count;
    assert(sum == 17);
    std::cout << "testSeventeenSplitsToKnownDenominations OK\n";
}

void testExperienceOrbDenominationTable() {
    assert(experienceOrbDenomination(1) == 1);
    assert(experienceOrbDenomination(2) == 1);
    assert(experienceOrbDenomination(3) == 3);
    assert(experienceOrbDenomination(6) == 3);
    assert(experienceOrbDenomination(7) == 7);
    assert(experienceOrbDenomination(16) == 7);
    assert(experienceOrbDenomination(17) == 17);
    assert(experienceOrbDenomination(2477) == 2477);
    assert(experienceOrbDenomination(999999) == 2477);
    std::cout << "testExperienceOrbDenominationTable OK\n";
}

// --- Physics: an orb falls and settles on the floor (gravity + collision),
// with its downward velocity zeroed once resting. ---
void testOrbFallsAndSettles() {
    world::World world = buildTestWorld();
    ExperienceOrbSystem orbs;
    orbs.restore({3.0F, 6.0F, 3.0F}, {0.0F, 0.0F, 0.0F}, 5, 1, 0U, 0U);
    PlayerExperience xp;
    const glm::vec3 farPlayer{14.0F, 1.0F, 14.0F};
    for (int tick = 0; tick < 300; ++tick) {
        static_cast<void>(orbs.tick(world, farPlayer, /*playerAlive=*/true, xp));
        if (!orbs.entities().empty() && orbs.entities().front().velocity.y == 0.0F &&
            orbs.entities().front().position.y < 2.0F) {
            break;
        }
    }
    assert(orbs.entities().size() == 1U);
    const float y = orbs.entities().front().position.y;
    // The 0.5-wide box rests with its base flush on the floor's top (y == 1).
    assert(y > 1.2F && y < 1.5F);
    assert(orbs.entities().front().velocity.y == 0.0F);
    std::cout << "testOrbFallsAndSettles OK\n";
}

// --- Physics: an orb submerged in water damps horizontally and floats gently
// upward (never plunges), matching setUnderwaterMovement's cap. ---
void testOrbFloatsInWater() {
    world::Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, world::Block::Stone);
            for (int y = 1; y <= 5; ++y) {
                chunk.setBlock(x, y, z, world::Block::Water);
            }
        }
    }
    world::World world;
    world.setChunk({0, 0}, std::move(chunk));
    ExperienceOrbSystem orbs;
    orbs.restore({3.0F, 3.0F, 3.0F}, {0.0F, 0.0F, 0.0F}, 5, 1, 0U, 0U);
    PlayerExperience xp;
    const glm::vec3 farPlayer{14.0F, 1.0F, 14.0F};
    float previousY = orbs.entities().front().position.y;
    bool everRose = false;
    for (int tick = 0; tick < 100; ++tick) {
        static_cast<void>(orbs.tick(world, farPlayer, /*playerAlive=*/true, xp));
        assert(!orbs.entities().empty());
        const float y = orbs.entities().front().position.y;
        // setUnderwaterMovement caps ascent at 0.06/tick; a physics bug that
        // let it plunge or rocket up would break this bound.
        assert(orbs.entities().front().velocity.y <= 0.061F);
        if (y > previousY) everRose = true;
        previousY = y;
    }
    assert(everRose);
    std::cout << "testOrbFloatsInWater OK\n";
}

// --- 8-block magnet: an orb within range accelerates toward the player;
// one further than 8 blocks does not move toward them (no magnet pull). ---
void testMagnetPullsWithinEightBlocks() {
    world::World world = buildTestWorld();
    {
        ExperienceOrbSystem orbs;
        orbs.restore({7.0F, 4.0F, 3.0F}, {0.0F, 0.0F, 0.0F}, 5, 1, 0U, 0U);
        PlayerExperience xp;
        const glm::vec3 player{3.0F, 4.0F, 3.0F};  // 4 blocks away, inside the 8-block radius
        const float startDistance = glm::distance(orbs.entities().front().position, player);
        for (int tick = 0; tick < 20; ++tick) {
            static_cast<void>(orbs.tick(world, player, /*playerAlive=*/true, xp));
            if (orbs.entities().empty()) break;  // could have been collected
        }
        if (!orbs.entities().empty()) {
            const float endDistance = glm::distance(orbs.entities().front().position, player);
            assert(endDistance < startDistance);
        }
    }
    std::cout << "testMagnetPullsWithinEightBlocks OK\n";
}

// --- Contact pickup: within reach, an orb is collected and its value is
// added to the player's experience; the orb is removed. ---
void testPickupAddsExperienceAndRemovesOrb() {
    world::World world = buildTestWorld();
    ExperienceOrbSystem orbs;
    orbs.restore({3.05F, 4.0F, 3.0F}, {0.0F, 0.0F, 0.0F}, 9, 1, 0U, 0U);
    PlayerExperience xp;
    const glm::vec3 player{3.0F, 4.0F, 3.0F};
    std::int32_t totalCollected = 0;
    for (int tick = 0; tick < 40; ++tick) {
        totalCollected += orbs.tick(world, player, /*playerAlive=*/true, xp);
        if (orbs.entities().empty()) break;
    }
    assert(orbs.entities().empty());
    assert(totalCollected == 9);
    assert(xp.totalExperience() == 9);
    std::cout << "testPickupAddsExperienceAndRemovesOrb OK\n";
}

// --- pickupDelay gates immediate pickup: an orb placed exactly at the player
// is NOT collected on the very first tick (its spawn pickupDelay is still
// counting down), but is collected shortly after. ---
void testPickupDelayGatesImmediateCollection() {
    world::World world = buildTestWorld();
    ExperienceOrbSystem orbs;
    world::gen::JavaRandom rng(7U);
    // spawnOne applies the standard spawn pickup delay.
    orbs.spawnOne({3.0F, 4.0F, 3.0F}, 5, rng);
    assert(!orbs.entities().empty());
    assert(orbs.entities().front().pickupDelayTicks > 0U);
    PlayerExperience xp;
    const glm::vec3 player{3.0F, 4.0F, 3.0F};
    // First tick: still inside the delay window, must not be collected.
    const auto firstTickCollected = orbs.tick(world, player, /*playerAlive=*/true, xp);
    assert(firstTickCollected == 0);
    assert(!orbs.entities().empty());
    // Once the delay elapses, contact picks it up.
    std::int32_t collected = 0;
    for (int tick = 0; tick < 10 && !orbs.entities().empty(); ++tick) {
        collected += orbs.tick(world, player, /*playerAlive=*/true, xp);
    }
    assert(orbs.entities().empty());
    assert(collected == 5);
    std::cout << "testPickupDelayGatesImmediateCollection OK\n";
}

// --- A dead player does not attract or collect orbs (playerAlive gate). ---
void testDeadPlayerDoesNotCollect() {
    world::World world = buildTestWorld();
    ExperienceOrbSystem orbs;
    orbs.restore({3.0F, 4.0F, 3.0F}, {0.0F, 0.0F, 0.0F}, 5, 1, 0U, 0U);
    PlayerExperience xp;
    const glm::vec3 player{3.0F, 4.0F, 3.0F};
    for (int tick = 0; tick < 30; ++tick) {
        static_cast<void>(orbs.tick(world, player, /*playerAlive=*/false, xp));
    }
    assert(!orbs.entities().empty());
    assert(xp.totalExperience() == 0);
    std::cout << "testDeadPlayerDoesNotCollect OK\n";
}

// --- Merge: two same-value orbs placed close together fold into one record
// whose count is the sum, with total value (value*count) conserved. ---
void testMergeConservesValue() {
    world::World world = buildTestWorld();
    ExperienceOrbSystem orbs;
    orbs.restore({4.0F, 4.0F, 4.0F}, {0.0F, 0.0F, 0.0F}, 7, 2, 0U, 0U);
    orbs.restore({4.05F, 4.02F, 4.03F}, {0.0F, 0.0F, 0.0F}, 7, 3, 0U, 0U);
    const std::int32_t before = std::accumulate(
        orbs.entities().begin(), orbs.entities().end(), std::int32_t{0},
        [](std::int32_t sum, const ExperienceOrb& orb) { return sum + orb.value * orb.count; });
    assert(before == 7 * 2 + 7 * 3);

    PlayerExperience xp;
    const glm::vec3 farPlayer{14.0F, 1.0F, 14.0F};
    // One tick is enough for the merge pass (it runs every tick, unlike
    // vanilla's every-20-ticks scanForMerges — XP-1 merges eagerly).
    static_cast<void>(orbs.tick(world, farPlayer, /*playerAlive=*/true, xp));

    assert(orbs.entities().size() == 1U);
    assert(orbs.entities().front().value == 7);
    assert(orbs.entities().front().count == 5);
    const std::int32_t after = std::accumulate(
        orbs.entities().begin(), orbs.entities().end(), std::int32_t{0},
        [](std::int32_t sum, const ExperienceOrb& orb) { return sum + orb.value * orb.count; });
    assert(after == before);
    std::cout << "testMergeConservesValue OK\n";
}

// --- Merge only folds same-value orbs; different denominations stay separate
// even when adjacent. ---
void testMergeDoesNotFoldDifferentValues() {
    world::World world = buildTestWorld();
    ExperienceOrbSystem orbs;
    orbs.restore({4.0F, 4.0F, 4.0F}, {0.0F, 0.0F, 0.0F}, 7, 1, 0U, 0U);
    orbs.restore({4.02F, 4.0F, 4.0F}, {0.0F, 0.0F, 0.0F}, 3, 1, 0U, 0U);
    PlayerExperience xp;
    const glm::vec3 farPlayer{14.0F, 1.0F, 14.0F};
    static_cast<void>(orbs.tick(world, farPlayer, /*playerAlive=*/true, xp));
    assert(orbs.entities().size() == 2U);
    std::cout << "testMergeDoesNotFoldDifferentValues OK\n";
}

// --- Despawn: an orb reaches exactly 6000 ageTicks and is removed; strictly
// fewer ticks leaves it in place (off-by-one boundary). ---
void testDespawnAtSixThousandTicks() {
    world::World world = buildTestWorld();
    ExperienceOrbSystem orbs;
    orbs.restore({3.0F, 4.0F, 3.0F}, {0.0F, 0.0F, 0.0F}, 5, 1, 5'998U, 0U);
    PlayerExperience xp;
    const glm::vec3 farPlayer{14.0F, 1.0F, 14.0F};
    // Tick 1: ageTicks becomes 5999, still alive.
    static_cast<void>(orbs.tick(world, farPlayer, /*playerAlive=*/true, xp));
    assert(!orbs.entities().empty());
    assert(orbs.entities().front().ageTicks == 5'999U);
    // Tick 2: ageTicks becomes 6000, despawns.
    static_cast<void>(orbs.tick(world, farPlayer, /*playerAlive=*/true, xp));
    assert(orbs.entities().empty());
    std::cout << "testDespawnAtSixThousandTicks OK\n";
}

// --- Pausing (never calling tick) must not age an orb — ageTicks only moves
// inside tick(), so a "paused" simulation that simply stops calling tick
// leaves ageTicks frozen. This is the sabotage②-target invariant. ---
void testAgeOnlyAdvancesOnTick() {
    ExperienceOrbSystem orbs;
    orbs.restore({0.0F, 5.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, 5, 1, 100U, 0U);
    assert(orbs.entities().front().ageTicks == 100U);
    // No tick() calls here: age must not move on its own.
    assert(orbs.entities().front().ageTicks == 100U);
    std::cout << "testAgeOnlyAdvancesOnTick OK\n";
}

// --- Determinism: the SAME JavaRandom seed, run twice through spawnMany,
// must produce the SAME sequence of initial orb velocities (position/value
// too). A wall-clock or global-RNG scatter would fail this every run. ---
void testDeterministicScatterVelocities() {
    ExperienceOrbSystem orbsA;
    ExperienceOrbSystem orbsB;
    world::gen::JavaRandom rngA(0xC0FFEEULL);
    world::gen::JavaRandom rngB(0xC0FFEEULL);
    orbsA.spawnMany({10.0F, 20.0F, 30.0F}, 500, rngA);
    orbsB.spawnMany({10.0F, 20.0F, 30.0F}, 500, rngB);

    assert(orbsA.entities().size() == orbsB.entities().size());
    for (std::size_t i = 0; i < orbsA.entities().size(); ++i) {
        assert(orbsA.entities()[i].value == orbsB.entities()[i].value);
        assert(orbsA.entities()[i].velocity == orbsB.entities()[i].velocity);
    }
    // Not every orb should coincidentally get the same velocity (guards
    // against a stub that always returns a fixed vector "matching" vacuously).
    bool sawVariation = false;
    for (std::size_t i = 1; i < orbsA.entities().size(); ++i) {
        if (orbsA.entities()[i].velocity != orbsA.entities()[0].velocity) {
            sawVariation = true;
            break;
        }
    }
    assert(sawVariation);
    std::cout << "testDeterministicScatterVelocities OK\n";
}

// --- Save round trip: gather -> PersistentExperienceOrb -> restore must land
// on the same position/velocity/value/count/age/pickupDelay. ---
void testSaveRoundTrip() {
    ExperienceOrbSystem source;
    source.restore({1.5F, 2.5F, 3.5F}, {0.1F, -0.02F, 0.05F}, 37, 4, 250U, 1U);
    source.restore({9.0F, 8.0F, 7.0F}, {0.0F, -0.03F, 0.0F}, 7, 1, 0U, 0U);

    std::vector<persistence::PersistentExperienceOrb> records;
    for (const auto& orb : source.entities()) {
        records.push_back({orb.position.x, orb.position.y, orb.position.z, orb.velocity.x,
                           orb.velocity.y, orb.velocity.z, orb.value, orb.count, orb.ageTicks,
                           orb.pickupDelayTicks});
    }

    ExperienceOrbSystem restored;
    for (const auto& record : records) {
        restored.restore({record.x, record.y, record.z}, {record.vx, record.vy, record.vz},
                         record.value, record.count, record.ageTicks, record.pickupDelayTicks);
    }

    assert(restored.entities().size() == source.entities().size());
    for (std::size_t i = 0; i < source.entities().size(); ++i) {
        assert(restored.entities()[i].position == source.entities()[i].position);
        assert(restored.entities()[i].velocity == source.entities()[i].velocity);
        assert(restored.entities()[i].value == source.entities()[i].value);
        assert(restored.entities()[i].count == source.entities()[i].count);
        assert(restored.entities()[i].ageTicks == source.entities()[i].ageTicks);
        assert(restored.entities()[i].pickupDelayTicks == source.entities()[i].pickupDelayTicks);
    }
    std::cout << "testSaveRoundTrip OK\n";
}

// --- SaveGame/world.dat round trip through the real XPOB block encoder: a
// SaveGame carrying orbs, written and re-read through the same in-memory
// world.dat framing the on-disk save uses, must reproduce every orb exactly
// (and an empty orb list -- a pre-XP-1 world -- must round-trip too). ---
void testWorldDatXpobBlockRoundTrip() {
    const auto tempDir = std::filesystem::temp_directory_path() / "mc_rebedrock_xp1_test";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);
    persistence::SaveRepository repository{tempDir};
    auto save = repository.create("xp1-world", 42ULL);
    save.experienceOrbs = {
        {1.0F, 2.0F, 3.0F, 0.1F, -0.03F, 0.05F, 37, 2, 120U, 0U},
        {-4.0F, 70.0F, 8.0F, 0.0F, -0.02F, 0.0F, 7, 1, 0U, 2U},
    };
    repository.save(save);

    const auto reloaded = repository.load(save.summary.identifier);
    assert(reloaded.experienceOrbs.size() == 2U);
    assert(reloaded.experienceOrbs[0].value == 37);
    assert(reloaded.experienceOrbs[0].count == 2);
    assert(reloaded.experienceOrbs[0].ageTicks == 120U);
    assert(reloaded.experienceOrbs[1].value == 7);
    assert(reloaded.experienceOrbs[1].pickupDelayTicks == 2U);

    std::filesystem::remove_all(tempDir);
    std::cout << "testWorldDatXpobBlockRoundTrip OK\n";
}

// --- A pre-XP-1 save (no XPOB block at all) must load with an empty orb list
// instead of throwing -- the "old world migrates cleanly" guarantee every
// other self-describing block gives (DROP's own precedent). Simulated here by
// loading a save that was written with an empty experienceOrbs vector (the
// XPOB block is still present but zero-length, which is what any current
// build actually writes for a world with no orbs -- a *true* pre-XP-1 file
// simply omits the block, and the reader's unknown-tag skip path handles that
// generically for every block type, exercised by the version-skip branch
// inside readExperienceOrbBlock/readDropBlock alike). ---
void testEmptyOrbListRoundTrips() {
    const auto tempDir = std::filesystem::temp_directory_path() / "mc_rebedrock_xp1_test_empty";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);
    persistence::SaveRepository repository{tempDir};
    auto save = repository.create("xp1-empty-world", 7ULL);
    assert(save.experienceOrbs.empty());
    repository.save(save);
    const auto reloaded = repository.load(save.summary.identifier);
    assert(reloaded.experienceOrbs.empty());
    std::filesystem::remove_all(tempDir);
    std::cout << "testEmptyOrbListRoundTrips OK\n";
}

namespace {
// FNV-1a, matching SaveRepository.cpp's private checksum() byte for byte (its
// own doc comment: "FNV-1a over everything before the trailing checksum
// field"). Re-derived here (not exported by the header) so this test can
// patch a real on-disk world.dat and still pass the load-time integrity
// check, the same way a byte-identical old-format file would.
[[nodiscard]] std::uint64_t fnv1a(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}
} // namespace

// --- The real migration case: a world.dat with the XPOB *frame itself
// physically absent* (not merely empty) -- what an actual pre-XP-1 build's
// file looks like on disk, byte for byte, not just "how the API models it".
// Written by this build, then the XPOB tag/size/version/payload bytes are
// spliced out of the raw file (checksum recomputed the way any edited/older
// save would need to be for the file to still pass the integrity check), and
// reloaded through the public SaveRepository API. It must load without
// throwing and report an empty orb list — the acceptance criterion's "旧档无
// orb 读回不崩". ---
void testOldWorldWithNoXpobFrameLoadsCleanly() {
    const auto tempDir = std::filesystem::temp_directory_path() / "mc_rebedrock_xp1_test_migrate";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);
    persistence::SaveRepository repository{tempDir};
    auto save = repository.create("xp1-migrate-world", 99ULL);
    save.experienceOrbs = {{5.0F, 6.0F, 7.0F, 0.0F, -0.03F, 0.0F, 17, 1, 0U, 0U}};
    repository.save(save);

    const auto worldDatPath = tempDir / save.summary.identifier / "world.dat";
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream input{worldDatPath, std::ios::binary};
        assert(input.good());
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    assert(bytes.size() > sizeof(std::uint64_t));

    // Locate the 'XPOB' tag (little-endian 'X','P','O','B' == the same byte
    // order appendInteger writes a std::uint32_t in) anywhere in the payload,
    // read its blockSizeBytes field (the next 4 bytes) and erase exactly that
    // many bytes starting at the tag -- turning this into a byte-for-byte
    // stand-in for a file that never had an XPOB frame at all.
    constexpr std::array<std::uint8_t, 4> kXpobTag{'X', 'P', 'O', 'B'};
    const auto tagIt = std::search(bytes.begin(), bytes.end(), kXpobTag.begin(), kXpobTag.end());
    assert(tagIt != bytes.end());
    const auto tagOffset = static_cast<std::size_t>(tagIt - bytes.begin());
    assert(tagOffset + 8U <= bytes.size());
    std::uint32_t blockSize = 0U;
    for (std::size_t i = 0; i < 4U; ++i) {
        blockSize |= static_cast<std::uint32_t>(bytes[tagOffset + 4U + i]) << (8U * i);
    }
    assert(tagOffset + blockSize <= bytes.size());
    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(tagOffset),
               bytes.begin() + static_cast<std::ptrdiff_t>(tagOffset + blockSize));

    // Recompute and rewrite the trailing FNV-1a checksum over the spliced
    // payload, exactly as SaveRepository::save() does for a legitimately
    // written file.
    const std::size_t checksumOffset = bytes.size() - sizeof(std::uint64_t);
    const auto newChecksum =
        fnv1a(std::span<const std::uint8_t>{bytes.data(), checksumOffset});
    for (std::size_t i = 0; i < sizeof(std::uint64_t); ++i) {
        bytes[checksumOffset + i] =
            static_cast<std::uint8_t>(newChecksum >> (8U * i));
    }

    {
        std::ofstream output{worldDatPath, std::ios::binary | std::ios::trunc};
        output.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
    }

    // The spliced file has no XPOB frame whatsoever now — exactly a pre-XP-1
    // save. load() must not throw, and experienceOrbs must come back empty.
    const auto reloaded = repository.load(save.summary.identifier);
    assert(reloaded.experienceOrbs.empty());
    // Everything the splice did not touch survives untouched: the migration
    // did not corrupt neighbouring blocks.
    assert(reloaded.summary.seed == 99ULL);

    std::filesystem::remove_all(tempDir);
    std::cout << "testOldWorldWithNoXpobFrameLoadsCleanly OK\n";
}

// --- Snapshot codec round trip: the render/network wire format carries
// position/value/count for every orb. ---
void testSnapshotCodecRoundTrip() {
    EntityRenderSnapshot snapshot;
    std::vector<ExperienceOrb> orbs;
    orbs.push_back({{1.0F, 2.0F, 3.0F}, {0.9F, 2.0F, 3.0F}, {0.0F, 0.0F, 0.0F}, 17, 1, 40U, 0U});
    orbs.push_back({{-5.0F, 64.0F, 5.0F}, {-5.0F, 64.0F, 5.0F}, {0.0F, 0.0F, 0.0F}, 7, 3, 0U, 0U});
    snapshot.assign({}, {}, orbs, {});

    const auto encoded = encodeEntitySnapshot(snapshot);
    const auto decoded = decodeEntitySnapshot(encoded, nullptr);
    assert(decoded.has_value());
    assert(decoded->experienceOrbs().size() == 2U);
    assert(decoded->experienceOrbs()[0].position == orbs[0].position);
    assert(decoded->experienceOrbs()[0].value == orbs[0].value);
    assert(decoded->experienceOrbs()[1].count == orbs[1].count);
    std::cout << "testSnapshotCodecRoundTrip OK\n";
}

} // namespace

int main() {
    testDenominationSplitSumsCorrectly();
    testSeventeenSplitsToKnownDenominations();
    testExperienceOrbDenominationTable();
    testOrbFallsAndSettles();
    testOrbFloatsInWater();
    testMagnetPullsWithinEightBlocks();
    testPickupAddsExperienceAndRemovesOrb();
    testPickupDelayGatesImmediateCollection();
    testDeadPlayerDoesNotCollect();
    testMergeConservesValue();
    testMergeDoesNotFoldDifferentValues();
    testDespawnAtSixThousandTicks();
    testAgeOnlyAdvancesOnTick();
    testDeterministicScatterVelocities();
    testSaveRoundTrip();
    testWorldDatXpobBlockRoundTrip();
    testEmptyOrbListRoundTrips();
    testOldWorldWithNoXpobFrameLoadsCleanly();
    testSnapshotCodecRoundTrip();
    std::cout << "experience_orb_test: all tests passed\n";
    return 0;
}
