#include "gameplay/EnvironmentSnapshot.hpp"
#include "gameplay/WorldSimulation.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"
#include "world/gen/Biome.hpp"

#include <stdexcept>
#include <string>
#include <utility>

// BM-2: rain, snow and ice follow the biome and the height.
//
// Level#isRainingAt used to be `raining && canSeeSky`, with a comment saying
// every biome in the game took rain — by then the game had deserts, snowy
// plains, the nether and the end. Nothing froze and nothing was ever dry.

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"biome_climate_test line " + std::to_string(line) +
                                 " failed: " + expression};
    }
}

#define REQUIRE(expression) require(static_cast<bool>(expression), #expression, __LINE__)

using mc::world::Block;
using mc::world::Chunk;
using mc::world::World;
using mc::world::kSeaLevel;
using mc::world::gen::Biome;
using mc::world::gen::Precipitation;
using mc::world::gen::precipitationAt;

// A desert is dry, a snowy plain gets snow and a plain gets rain — all three at
// once, in the same downpour, at the same height.
void checkPrecipitationByBiome() {
    constexpr int kY = kSeaLevel + 1;
    REQUIRE(precipitationAt(Biome::Desert, 8, kY, 8, kSeaLevel) == Precipitation::None);
    REQUIRE(precipitationAt(Biome::Savanna, 8, kY, 8, kSeaLevel) == Precipitation::None);
    REQUIRE(precipitationAt(Biome::SnowyPlains, 8, kY, 8, kSeaLevel) == Precipitation::Snow);
    REQUIRE(precipitationAt(Biome::Plains, 8, kY, 8, kSeaLevel) == Precipitation::Rain);
    REQUIRE(precipitationAt(Biome::Forest, 8, kY, 8, kSeaLevel) == Precipitation::Rain);
    // The nether and the end have no weather at all.
    REQUIRE(precipitationAt(Biome::NetherWastes, 8, kY, 8, kSeaLevel) == Precipitation::None);
    REQUIRE(precipitationAt(Biome::TheEnd, 8, kY, 8, kSeaLevel) == Precipitation::None);
}

// The snow line: one biome is rainy at its foot and snowy at its peak, which is
// the whole reason getHeightAdjustedTemperature exists.
void checkSnowLine() {
    // Taiga sits at 0.25, comfortably above the 0.15 rain threshold at sea level
    // and below it once the lapse rate has taken enough off.
    REQUIRE(precipitationAt(Biome::Taiga, 40, kSeaLevel + 4, 40, kSeaLevel) == Precipitation::Rain);
    bool foundSnowLine = false;
    for (int y = kSeaLevel + 17; y <= kSeaLevel + 200 && !foundSnowLine; ++y) {
        if (precipitationAt(Biome::Taiga, 40, y, 40, kSeaLevel) == Precipitation::Snow) {
            foundSnowLine = true;
        }
    }
    REQUIRE(foundSnowLine);
    // Below the snow level (sea level + 17) the temperature is the biome's own,
    // untouched: the lapse rate starts above it, not at sea level.
    REQUIRE(mc::world::gen::heightAdjustedTemperature(Biome::Taiga, 40, kSeaLevel, 40, kSeaLevel) ==
            mc::world::gen::heightAdjustedTemperature(Biome::Taiga, 40, kSeaLevel + 17, 40,
                                                      kSeaLevel));
    // And above it the temperature only falls.
    const float atLine =
        mc::world::gen::heightAdjustedTemperature(Biome::Taiga, 40, kSeaLevel + 17, 40, kSeaLevel);
    const float highUp =
        mc::world::gen::heightAdjustedTemperature(Biome::Taiga, 40, kSeaLevel + 120, 40, kSeaLevel);
    REQUIRE(highUp < atLine);
    // Vanilla's lapse rate is (noise * 8 + dy) * 0.05 / 40 per block, so 100
    // blocks is roughly 0.125 of temperature before the noise term. Anything
    // wildly off this — a missing divisor, say — fails here.
    REQUIRE(atLine - highUp > 0.08F);
    REQUIRE(atLine - highUp < 0.20F);
}

// A dry biome never pays for the noise sample, and its answer does not depend on
// height at all.
void checkDryBiomeShortCircuits() {
    for (int y = kSeaLevel; y < kSeaLevel + 200; y += 37) {
        REQUIRE(precipitationAt(Biome::Desert, 12, y, 12, kSeaLevel) == Precipitation::None);
    }
}

// A pond at the surface with a shore: the eastern half of every chunk is stone.
// Vanilla only freezes a cell that is not water on all four sides, so a sheet of
// water with no edge never ices over — a pond without a shore would test nothing.
World makePondWorld(Biome biome, Block surface) {
    World world;
    for (int chunkZ = -1; chunkZ <= 1; ++chunkZ) {
        for (int chunkX = -1; chunkX <= 1; ++chunkX) {
            Chunk chunk;
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    chunk.setBlock(x, mc::world::kMinY + 1, z, Block::Stone);
                    chunk.setBlock(x, mc::world::kMinY + 2, z, x < 8 ? surface : Block::Stone);
                    chunk.setColumnBiome(x, z, biome);
                }
            }
            world.setChunk({chunkX, chunkZ}, std::move(chunk));
        }
    }
    return world;
}

[[nodiscard]] int countBlocks(const World& world, Block block, int y) {
    int count = 0;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            if (world.block(x, y, z) == block) {
                ++count;
            }
        }
    }
    return count;
}

// Water freezes in a cold biome and does not in a temperate one. Both worlds are
// identical apart from the biome, and both run the same number of ticks.
void checkFreezing() {
    constexpr int kSurfaceY = mc::world::kMinY + 2;
    constexpr int kTicks = 4000;
    {
        World cold = makePondWorld(Biome::SnowyPlains, Block::Water);
        mc::gameplay::WorldSimulation simulation;
        for (int tick = 0; tick < kTicks; ++tick) {
            static_cast<void>(simulation.tick(cold));
        }
        REQUIRE(countBlocks(cold, Block::Ice, kSurfaceY) > 0);
    }
    {
        World warm = makePondWorld(Biome::Plains, Block::Water);
        mc::gameplay::WorldSimulation simulation;
        for (int tick = 0; tick < kTicks; ++tick) {
            static_cast<void>(simulation.tick(warm));
        }
        REQUIRE(countBlocks(warm, Block::Ice, kSurfaceY) == 0);
    }
    // Freezing is part of vanilla's tickChunk but is not a random tick, so
    // /gamerule randomTickSpeed 0 must not stop it.
    {
        World cold = makePondWorld(Biome::SnowyPlains, Block::Water);
        mc::gameplay::WorldSimulation simulation;
        simulation.setRandomTickSpeed(0);
        for (int tick = 0; tick < kTicks; ++tick) {
            static_cast<void>(simulation.tick(cold));
        }
        REQUIRE(countBlocks(cold, Block::Ice, kSurfaceY) > 0);
    }
    // A cold biome with no water freezes nothing.
    {
        World dry = makePondWorld(Biome::SnowyPlains, Block::Stone);
        mc::gameplay::WorldSimulation simulation;
        for (int tick = 0; tick < kTicks; ++tick) {
            static_cast<void>(simulation.tick(dry));
        }
        REQUIRE(countBlocks(dry, Block::Ice, kSurfaceY) == 0);
    }
}

// Level#isRainingAt now asks the biome. Rain reaching a desert was what let a
// desert's farmland stay moist through a storm.
void checkIsRainingAtGate() {
    mc::gameplay::EnvironmentSnapshot raining{};
    raining.raining = true;
    raining.skyLightLevel = 15;
    constexpr int kAbove = mc::world::kMinY + 3;
    // canSeeSky reads the stored sky light, which a hand-built world does not
    // compute; open the cell under test explicitly so the assertions below are
    // about the biome gate and nothing else.
    const auto openSky = [](World& world) {
        world.setSkyLight(8, kAbove, 8, 15U);
    };
    World desert = makePondWorld(Biome::Desert, Block::Stone);
    World plains = makePondWorld(Biome::Plains, Block::Stone);
    World snowy = makePondWorld(Biome::SnowyPlains, Block::Stone);
    openSky(desert);
    openSky(plains);
    openSky(snowy);
    REQUIRE(!mc::gameplay::isRainingAt(desert, 8, kAbove, 8, raining));
    REQUIRE(mc::gameplay::isRainingAt(plains, 8, kAbove, 8, raining));
    // A snowy biome gets snow, which is not rain: farmland there does not wet.
    REQUIRE(!mc::gameplay::isRainingAt(snowy, 8, kAbove, 8, raining));
    // And nothing falls when it is not raining at all.
    mc::gameplay::EnvironmentSnapshot clear{};
    clear.skyLightLevel = 15;
    REQUIRE(!mc::gameplay::isRainingAt(plains, 8, kAbove, 8, clear));
}

} // namespace

int main() {
    checkPrecipitationByBiome();
    checkSnowLine();
    checkDryBiomeShortCircuits();
    checkFreezing();
    checkIsRainingAtGate();
    return 0;
}
