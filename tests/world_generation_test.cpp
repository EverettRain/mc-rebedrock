#include "world/SurfaceGenerator.hpp"

#include "world/gen/JavaRandom.hpp"
#include "world/gen/NoiseSampler.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <map>

namespace {

using mc::world::Block;
using mc::world::Chunk;
using mc::world::kSeaLevel;
using mc::world::kWorldHeight;

[[nodiscard]] int surfaceHeight(const Chunk& chunk, int x, int z) {
    for (int y = kWorldHeight - 1; y >= 0; --y) {
        const auto block = chunk.block(x, y, z);
        if (block != Block::Air && block != Block::Water) {
            return y;
        }
    }
    return -1;
}

} // namespace

int main() {
    // java.util.Random, against values a real JVM produces for seed 42. Every
    // generator below rides on this, so it is checked first.
    {
        mc::world::gen::JavaRandom random{42U};
        assert(random.nextInt() == -1170105035);
        assert(random.nextInt() == 234785527);
        mc::world::gen::JavaRandom bounded{42U};
        assert(bounded.nextInt(100) == 30);
        assert(bounded.nextInt(100) == 63);
        assert(bounded.nextInt(100) == 48);
        mc::world::gen::JavaRandom longs{42U};
        assert(longs.nextLong() == -5025562857975149833LL);
        mc::world::gen::JavaRandom floats{42U};
        assert(std::abs(floats.nextFloat() - 0.7275636792182922F) < 1e-7F);
        mc::world::gen::JavaRandom doubles{42U};
        assert(std::abs(doubles.nextDouble() - 0.7275636800328681) < 1e-12);
        // A power-of-two bound takes the fast path; both paths must stay in range.
        mc::world::gen::JavaRandom bounds{7U};
        for (int index = 0; index < 500; ++index) {
            const int power = bounds.nextInt(16);
            const int odd = bounds.nextInt(13);
            assert(power >= 0 && power < 16);
            assert(odd >= 0 && odd < 13);
        }
    }

    // The Perlin stack is deterministic for a seed and stays inside the band the
    // biome thresholds are calibrated against.
    {
        mc::world::gen::JavaRandom first{1234U};
        mc::world::gen::JavaRandom second{1234U};
        const mc::world::gen::OctavePerlinNoiseSampler a{first, 4};
        const mc::world::gen::OctavePerlinNoiseSampler b{second, 4};
        double minimum = 1.0;
        double maximum = -1.0;
        for (int index = 0; index < 4000; ++index) {
            const double x = static_cast<double>(index) * 0.017;
            const double z = static_cast<double>(index) * 0.031;
            const double sample = a.sample(x, 0.0, z);
            assert(sample == b.sample(x, 0.0, z));
            minimum = std::min(minimum, sample);
            maximum = std::max(maximum, sample);
        }
        assert(minimum > -1.0 && maximum < 1.0);
        assert(maximum - minimum > 0.2);
    }

    const mc::world::SurfaceGenerator generator{0xC0FFEEULL};

    // Generation is a pure function of the seed and the chunk position.
    {
        const auto first = generator.generate(3, -5);
        const auto repeated = generator.generate(3, -5);
        for (int y = 0; y < kWorldHeight; ++y) {
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    assert(first.block(x, y, z) == repeated.block(x, y, z));
                }
            }
        }
    }

    // Survey a wide area: terrain has to land in a playable band, sit on
    // bedrock, and carry the vanilla ore set at roughly the vanilla rates.
    std::map<Block, long> blocks;
    long caveAir = 0;
    long undergroundCells = 0;
    // Gravel sitting directly under a cave's air: the patchy ore-blob kind is a
    // couple of hundred cells over this survey, whereas the surface pass's
    // "underwater" branch paves every buried cave floor with it (thousands).
    long caveFloorGravel = 0;
    int lowestSurface = kWorldHeight;
    int highestSurface = 0;
    // A 12x12 survey keeps the cave-fraction and ore-count assertions stable:
    // a smaller region can land on a locally cave-poor spot now that the
    // carver runs with vanilla's 112-step tunnel budget.
    constexpr int kChunks = 12;
    for (int chunkZ = 0; chunkZ < kChunks; ++chunkZ) {
        for (int chunkX = 0; chunkX < kChunks; ++chunkX) {
            const auto chunk = generator.generate(chunkX, chunkZ);
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    // ChunkGenerator#buildBedrock always fills the bottom layer.
                    assert(chunk.block(x, 0, z) == Block::Bedrock);
                    const int height = surfaceHeight(chunk, x, z);
                    assert(height > 0);
                    lowestSurface = std::min(lowestSurface, height);
                    highestSurface = std::max(highestSurface, height);
                    for (int y = 0; y < kWorldHeight; ++y) {
                        const auto block = chunk.block(x, y, z);
                        ++blocks[block];
                        if (y >= 6 && y <= 50) {
                            ++undergroundCells;
                            if (block == Block::Air) {
                                ++caveAir;
                            } else if (block == Block::Gravel &&
                                       chunk.block(x, y + 1, z) == Block::Air) {
                                ++caveFloorGravel;
                            }
                        }
                    }
                }
            }
        }
    }

    assert(lowestSurface > 20);
    assert(highestSurface < kWorldHeight - 8);
    // Real relief rather than a plate: the survey has to span at least a dozen
    // blocks of height.
    assert(highestSurface - lowestSurface > 12);

    // The carvers have to open real space underground without hollowing it out.
    const double caveFraction =
        static_cast<double>(caveAir) / static_cast<double>(undergroundCells);
    assert(caveFraction > 0.02);
    assert(caveFraction < 0.30);

    // Cave floors stay bare stone. The surface pass runs after carving, and its
    // "underwater" branch must only fire on real sea floors (water above);
    // without that gate it paves every buried cave floor with a uniform gravel
    // sheet and this count runs to the thousands, not the ~150 ore-blob cells.
    assert(caveFloorGravel < 1500);

    // Every ore in DefaultBiomeFeatures#addDefaultOres shows up, and the
    // per-chunk counts stay within a factor of two of the vanilla averages
    // (coal ~142, iron ~78, gold ~9, diamond ~3.7, lapis ~3.4, redstone ~29).
    constexpr double chunkCount = kChunks * kChunks;
    const auto perChunk = [&](Block block) {
        return static_cast<double>(blocks[block]) / chunkCount;
    };
    assert(perChunk(Block::CoalOre) > 70.0 && perChunk(Block::CoalOre) < 290.0);
    assert(perChunk(Block::IronOre) > 39.0 && perChunk(Block::IronOre) < 160.0);
    assert(perChunk(Block::GoldOre) > 4.0 && perChunk(Block::GoldOre) < 20.0);
    assert(perChunk(Block::DiamondOre) > 1.0 && perChunk(Block::DiamondOre) < 8.0);
    assert(perChunk(Block::LapisOre) > 1.0 && perChunk(Block::LapisOre) < 8.0);
    assert(perChunk(Block::RedstoneOre) > 12.0 && perChunk(Block::RedstoneOre) < 60.0);
    assert(blocks[Block::Granite] > 0);
    assert(blocks[Block::Diorite] > 0);
    assert(blocks[Block::Andesite] > 0);
    assert(blocks[Block::Gravel] > 0);

    // The surface builder ran: the ground is grass over dirt, not raw stone.
    assert(blocks[Block::Grass] > 0);
    assert(blocks[Block::Dirt] > 0);

    // Biomes drive the wood set, and oceans are a continent apart, so this scan
    // has to span far more ground than the survey above. It has to turn up at
    // least two tree species, each with its own leaves, and standing water.
    std::map<Block, long> logs;
    long generatedWater = 0;
    for (int chunkZ = -40; chunkZ <= 40; chunkZ += 5) {
        for (int chunkX = -40; chunkX <= 40; chunkX += 5) {
            const auto chunk = generator.generate(chunkX, chunkZ);
            for (int y = 0; y < kWorldHeight; ++y) {
                for (int z = 0; z < 16; ++z) {
                    for (int x = 0; x < 16; ++x) {
                        const auto block = chunk.block(x, y, z);
                        if (block == Block::Water) {
                            assert(y <= kSeaLevel);
                            ++generatedWater;
                        }
                        if (y < kSeaLevel) {
                            continue;
                        }
                        if (mc::world::isLog(block) || mc::world::isLeaves(block)) {
                            ++logs[block];
                        }
                    }
                }
            }
        }
    }
    assert(generatedWater > 0);
    int logSpecies = 0;
    int leafSpecies = 0;
    for (const auto& [block, count] : logs) {
        assert(count > 0);
        if (mc::world::isLog(block)) ++logSpecies;
        if (mc::world::isLeaves(block)) ++leafSpecies;
    }
    assert(logSpecies >= 2);
    assert(leafSpecies >= 2);
    // A tree's leaves always belong to its own wood set.
    assert(logs[Block::SpruceLog] == 0 || logs[Block::SpruceLeaves] > 0);
    assert(logs[Block::BirchLog] == 0 || logs[Block::BirchLeaves] > 0);

    // The biome map itself has to produce more than one biome, and answer the
    // same way for the same column every time.
    std::map<int, int> biomes;
    for (int z = -2000; z <= 2000; z += 64) {
        for (int x = -2000; x <= 2000; x += 64) {
            const auto biome = generator.biomeAt(x, z);
            assert(biome == generator.biomeAt(x, z));
            ++biomes[static_cast<int>(biome)];
        }
    }
    assert(biomes.size() >= 4);
    return 0;
}
