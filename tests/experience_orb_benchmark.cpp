// Informational benchmark for the XP-1 experience orb pool's per-tick cost:
// physics + the 8-block player magnet + the O(n^2) same-value merge scan +
// contact pickup, at a population heavier than ordinary play (200 orbs — a
// mob farm or a big mining haul spawning many at once, vanilla's own
// ORB_GROUPS_PER_AREA=40-per-scan-bucket cap notwithstanding). Wall-clock
// values vary by machine; the checksum keeps the simulated work observable so
// a future change to the tick loop can be compared against this run.

#include "gameplay/PlayerExperience.hpp"
#include "gameplay/entities/ExperienceOrb.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/gen/JavaRandom.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace mc;

mc::world::World makeFlatWorld(int chunkRadius) {
    mc::world::World world;
    for (int chunkZ = -chunkRadius; chunkZ <= chunkRadius; ++chunkZ) {
        for (int chunkX = -chunkRadius; chunkX <= chunkRadius; ++chunkX) {
            mc::world::Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    chunk.setBlock(x, 0, z, mc::world::Block::Stone);
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
        }
    }
    return world;
}

struct Measurement final {
    double millisecondsPerTick = 0.0;
    std::uint64_t checksum = 0U;
};

Measurement run(std::size_t orbCount, int ticks) {
    world::World world = makeFlatWorld(2);
    gameplay::ExperienceOrbSystem orbs;
    gameplay::PlayerExperience xp;
    world::gen::JavaRandom rng(0xE7BEEF00ULL);
    // Scattered across a small area at various heights so gravity, the magnet
    // and the merge scan all do real work every tick, matching a mob-farm
    // burst rather than a synthetic all-identical layout.
    for (std::size_t i = 0; i < orbCount; ++i) {
        const glm::vec3 position{
            8.0F + static_cast<float>(rng.nextInt(20)) - 10.0F,
            10.0F + static_cast<float>(rng.nextInt(10)),
            8.0F + static_cast<float>(rng.nextInt(20)) - 10.0F,
        };
        const std::int32_t value = gameplay::experienceOrbDenomination(1 + rng.nextInt(40));
        orbs.spawnOne(position, value, rng);
    }

    const glm::vec3 playerPosition{8.0F, 5.0F, 8.0F};
    std::uint64_t checksum = 0U;
    const auto start = std::chrono::steady_clock::now();
    for (int tick = 0; tick < ticks; ++tick) {
        const auto collected = orbs.tick(world, playerPosition, /*playerAlive=*/true, xp);
        checksum += static_cast<std::uint64_t>(collected) + orbs.entities().size();
    }
    const auto end = std::chrono::steady_clock::now();
    const double totalMs =
        std::chrono::duration<double, std::milli>(end - start).count();
    return {totalMs / static_cast<double>(ticks), checksum};
}

} // namespace

int main() {
    constexpr int kTicks = 2000;
    for (const std::size_t population : {10UL, 50UL, 200UL}) {
        const auto measurement = run(population, kTicks);
        std::cout << "orbs=" << population << " ms/tick=" << measurement.millisecondsPerTick
                  << " checksum=" << measurement.checksum << "\n";
    }
    return 0;
}
