// MDL-3: the snow layer — the LAYERS axis, its two shapes, its support rule, its
// melt, its drop, and the accumulation half of tickPrecipitation.
//
// The half that matters most here is the one that is easy to get subtly wrong:
// a snow layer's COLLISION shape is one layer shorter than what it draws (26.1
// getCollisionShape reads SHAPES[layers - 1] while getShape reads SHAPES[layers]).
// That is the difference between snow you walk over and snow you trip on, and
// nothing about the block's appearance would reveal it.
//
// The accumulation test drives the real WorldSimulation tick rather than calling
// the private half directly: what is being checked is that the snow half runs
// independently of the freezing half, under the rain gate and the gamerule
// ceiling, which is a property of the whole tick and not of one function.

#include "gameplay/MiningSystem.hpp"
#include "gameplay/WorldSimulation.hpp"
#include "world/Block.hpp"
#include "world/BlockPlacement.hpp"
#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <utility>

namespace {

using mc::world::Block;
using mc::world::BlockOrientation;
using mc::world::BlockState;
using mc::world::Chunk;
using mc::world::World;

[[nodiscard]] bool near(float a, float b) { return std::fabs(a - b) < 1.0e-6F; }

// A stone floor at y = 0 across one chunk, in a biome cold enough to snow.
[[nodiscard]] World snowyWorld() {
    World world;
    Chunk chunk;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            chunk.setBlock(x, 0, z, Block::Stone);
            chunk.setColumnBiome(x, z, mc::world::gen::Biome::SnowyPlains);
        }
    }
    world.setChunk({0, 0}, std::move(chunk));
    return world;
}

// Runs enough ticks that the 1-in-16 column draw is overwhelmingly likely to
// have hit the whole chunk. The draw is deterministic (the simulation owns its
// own LCG), so this is a fixed number, not a flake.
void runPrecipitationTicks(mc::gameplay::WorldSimulation& simulation, World& world, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        static_cast<void>(simulation.tick(world));
    }
}

[[nodiscard]] int snowCells(const World& world) {
    int count = 0;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            if (world.block(x, 1, z) == Block::Snow) {
                ++count;
            }
        }
    }
    return count;
}

} // namespace

int main() {
    using mc::world::blockShape;
    using mc::world::collisionShape;

    // ---------------------------------------------------------------------
    // 1) Two shapes, and they differ by exactly one layer.
    // ---------------------------------------------------------------------
    {
        for (int layers = 1; layers <= 8; ++layers) {
            const auto state = BlockState{Block::Snow}.withLayers(layers);
            assert(state.layers() == layers);
            const auto visual = blockShape(state);
            assert(visual.kind == mc::world::ShapeKind::Column);
            assert(near(visual.top, static_cast<float>(layers) * 2.0F / 16.0F));
            const auto collision = collisionShape(state);
            assert(near(collision.top, static_cast<float>(layers - 1) * 2.0F / 16.0F));
        }
        // One layer draws 2/16 and collides with nothing at all: it is walked
        // straight over, which is the whole reason the two tables differ.
        assert(near(blockShape(BlockState{Block::Snow}).top, 2.0F / 16.0F));
        assert(near(collisionShape(BlockState{Block::Snow}).top, 0.0F));
        // Eight layers is a full cell to the eye and 14/16 to the body.
        const auto full = BlockState{Block::Snow}.withLayers(8);
        assert(near(blockShape(full).top, 1.0F));
        assert(near(collisionShape(full).top, 14.0F / 16.0F));
        // The default state is one layer (vanilla's registerDefaultState), which
        // is what makes "place snow" mean "place one layer".
        assert(BlockState{Block::Snow}.layers() == 1);
        // withLayers clamps rather than wrapping the packed value.
        assert(BlockState{Block::Snow}.withLayers(0).layers() == 1);
        assert(BlockState{Block::Snow}.withLayers(99).layers() == 8);
    }

    // ---------------------------------------------------------------------
    // 2) Support: a full face below, or eight layers of snow below — and
    //    nothing else. The eight-layer clause is why this is its own rule.
    // ---------------------------------------------------------------------
    {
        World world = snowyWorld();
        assert(mc::world::canBlockSurvive(world, {4, 1, 4}, Block::Snow, BlockOrientation::Up));
        assert(!mc::world::canBlockSurvive(world, {4, 6, 4}, Block::Snow, BlockOrientation::Up));

        world.setState(4, 1, 4, BlockState{Block::Snow});
        assert(!mc::world::canBlockSurvive(world, {4, 2, 4}, Block::Snow, BlockOrientation::Up));
        world.setState(4, 1, 4, BlockState{Block::Snow}.withLayers(8));
        assert(mc::world::canBlockSurvive(world, {4, 2, 4}, Block::Snow, BlockOrientation::Up));
    }

    // ---------------------------------------------------------------------
    // 3) Melt: BLOCK light above 11, not sky light. A snowy field under an open
    //    noon sky must stay put — that is the whole point of the distinction.
    // ---------------------------------------------------------------------
    {
        World world = snowyWorld();
        mc::gameplay::WorldSimulation simulation;
        simulation.setRandomTickSpeed(1000); // draw hard so the cell is hit
        world.setState(4, 1, 4, BlockState{Block::Snow}.withLayers(3));
        static_cast<void>(world.setBlockLight(4, 1, 4, 11U));
        runPrecipitationTicks(simulation, world, 40);
        assert(world.block(4, 1, 4) == Block::Snow); // 11 is not "above 11"

        static_cast<void>(world.setBlockLight(4, 1, 4, 12U));
        runPrecipitationTicks(simulation, world, 40);
        assert(world.block(4, 1, 4) == Block::Air);
    }

    // ---------------------------------------------------------------------
    // 4) Drops: one snowball per layer.
    // ---------------------------------------------------------------------
    {
        std::uint64_t random = 1U;
        const mc::gameplay::ItemStack shovel{Block::Air, 1U,
                                             &mc::gameplay::items::IronShovel};
        for (const int layers : {1, 3, 8}) {
            const auto drops =
                mc::gameplay::minedDrops(Block::Snow, shovel, random, 0, false, layers);
            assert(drops.count == 1U);
            assert(drops.view()[0].item == &mc::gameplay::items::Snowball);
            assert(drops.view()[0].count == static_cast<std::uint8_t>(layers));
        }
    }

    // ---------------------------------------------------------------------
    // 5) Accumulation: the snow half of tickPrecipitation, which this build
    //    never had. Gated on rain, on the gamerule ceiling, and on the light.
    // ---------------------------------------------------------------------
    {
        // Not raining: nothing settles, however cold the biome is.
        World dry = snowyWorld();
        mc::gameplay::WorldSimulation simulation;
        mc::gameplay::EnvironmentSnapshot clear;
        clear.raining = false;
        simulation.setEnvironment(clear);
        runPrecipitationTicks(simulation, dry, 400);
        assert(snowCells(dry) == 0);

        // Raining and cold: snow settles, one layer deep, because the gamerule
        // default is 1 — the same default vanilla ships.
        World world = snowyWorld();
        mc::gameplay::WorldSimulation snowing;
        mc::gameplay::EnvironmentSnapshot rain;
        rain.raining = true;
        snowing.setEnvironment(rain);
        assert(snowing.maximumSnowAccumulation() == 1);
        runPrecipitationTicks(snowing, world, 600);
        const int settled = snowCells(world);
        assert(settled > 0);
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                if (world.block(x, 1, z) == Block::Snow) {
                    assert(world.state(x, 1, z).layers() == 1); // the ceiling holds
                }
            }
        }

        // Raise the ceiling and the same columns keep piling up.
        snowing.setMaximumSnowAccumulation(4);
        runPrecipitationTicks(snowing, world, 3000);
        int deepest = 0;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                if (world.block(x, 1, z) == Block::Snow) {
                    const int layers = world.state(x, 1, z).layers();
                    assert(layers <= 4);
                    deepest = layers > deepest ? layers : deepest;
                }
            }
        }
        assert(deepest > 1);

        // A warm biome never accumulates, however hard it rains.
        World warm;
        Chunk warmChunk;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                warmChunk.setBlock(x, 0, z, Block::Stone);
                warmChunk.setColumnBiome(x, z, mc::world::gen::Biome::Plains);
            }
        }
        warm.setChunk({0, 0}, std::move(warmChunk));
        mc::gameplay::WorldSimulation warmSimulation;
        warmSimulation.setEnvironment(rain);
        runPrecipitationTicks(warmSimulation, warm, 400);
        assert(snowCells(warm) == 0);
    }

    std::cout << "snow_layer_test passed\n";
    return 0;
}
