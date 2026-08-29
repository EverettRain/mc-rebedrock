// Informational benchmark for the full creature-AI path at the vanilla
// MONSTER cap (70). It compares identical moving/collision simulation with an
// empty Brain against zombies continuously pursuing a moving survival player.
// Wall-clock values vary by machine; the checksum keeps the simulation work
// observable and the output makes regressions easy to compare locally.

#include "gameplay/Difficulty.hpp"
#include "gameplay/EntitySystem.hpp"
#include "gameplay/entities/EntityRegistry.hpp"
#include "gameplay/entities/EntityType.hpp"
#include "gameplay/entities/MobBrain.hpp"
#include "gameplay/entities/BuiltinSpecies.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/World.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <utility>

namespace {

class NoGoalAi final : public mc::gameplay::entities::EntityAi {
  public:
    void configureBrain(mc::gameplay::entities::MobBrain&) const override {}
};

[[nodiscard]] const mc::gameplay::entities::EntityType& baselineType() {
    static const NoGoalAi ai;
    static const auto type = mc::gameplay::entities::EntityType::Builder::create(
                                 mc::gameplay::entities::MobCategory::Creature, ai)
                                 .sized(0.6F, 1.95F)
                                 .health(20.0F)
                                 .movementSpeed(0.046F)
                                 .followRange(35.0F)
                                 .build("mob_ai_benchmark_baseline");
    return type;
}

[[nodiscard]] mc::world::World makeFlatWorld(int chunkRadius) {
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
    std::size_t attacks = 0U;
    std::uint64_t searches = 0U;
    std::uint64_t expandedNodes = 0U;
    std::uint64_t simulatedTicks = 0U;
    double checksum = 0.0;
};

struct SearchStressMeasurement final {
    double millisecondsPerSearch = 0.0;
    double expandedNodesPerSearch = 0.0;
};

[[nodiscard]] Measurement runScenario(const mc::world::World& world,
                                      const mc::gameplay::entities::EntityType& type) {
    constexpr std::size_t kMobs = 70U;
    constexpr int kWarmupTicks = 100;
    constexpr int kMeasuredTicks = 500;
    constexpr float kTau = 6.28318530718F;
    mc::gameplay::EntitySystem entities;
    for (std::size_t index = 0; index < kMobs; ++index) {
        const float angle = kTau * static_cast<float>(index) / static_cast<float>(kMobs);
        const float radius = 22.0F + static_cast<float>(index % 5U);
        entities.spawn({std::sin(angle) * radius + 0.5F, 1.001F, std::cos(angle) * radius + 0.5F},
                       type, static_cast<std::uint32_t>(1000U + index));
    }

    Measurement measurement;
    measurement.simulatedTicks = kWarmupTicks + kMeasuredTicks;
    const auto runTick = [&](int tick) {
        const float angle = static_cast<float>(tick) * 0.035F;
        const glm::vec3 player{std::sin(angle) * 8.0F + 0.5F, 1.001F,
                               std::cos(angle) * 8.0F + 0.5F};
        const auto result = entities.tick(world, player, 0.6F, 1.8F,
                                          mc::gameplay::Difficulty::Normal, true, false, 48.0F);
        measurement.attacks += result.mobAttacks.size();
    };

    for (int tick = 0; tick < kWarmupTicks; ++tick) {
        runTick(tick);
    }
    const auto started = std::chrono::steady_clock::now();
    for (int tick = 0; tick < kMeasuredTicks; ++tick) {
        runTick(kWarmupTicks + tick);
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    measurement.millisecondsPerTick = std::chrono::duration<double, std::milli>(elapsed).count() /
                                      static_cast<double>(kMeasuredTicks);
    for (const auto& entity : entities.entities()) {
        measurement.checksum += static_cast<double>(entity.position.x) * 0.25 +
                                static_cast<double>(entity.position.z) * 0.5;
        const auto& search = entity.brain.navigation().cumulativeSearchStats();
        measurement.searches += search.searches;
        measurement.expandedNodes += search.expandedNodes;
    }
    return measurement;
}

[[nodiscard]] SearchStressMeasurement runBoundedSearchStress() {
    constexpr int kSearches = 100;
    auto world = makeFlatWorld(3);
    // Split every loaded row with a solid three-high wall. The target remains a
    // valid standable node, but no route reaches it, forcing the bounded search
    // close to its 1024-node ceiling.
    for (int z = -48; z < 64; ++z) {
        for (int y = 1; y <= 3; ++y) {
            static_cast<void>(world.setBlock(0, y, z, mc::world::Block::Stone));
        }
    }
    mc::gameplay::EntitySystem entities;
    entities.spawn({-20.5F, 1.001F, 0.5F}, mc::gameplay::entities::builtinSpecies("zombie"), 9001U);
    const auto& entity = entities.entities().front();
    mc::gameplay::entities::GroundNavigation navigation;
    const auto started = std::chrono::steady_clock::now();
    for (int search = 0; search < kSearches; ++search) {
        static_cast<void>(navigation.canReach(world, entity, {20.5F, 1.001F, 0.5F}));
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto& stats = navigation.cumulativeSearchStats();
    return {
        std::chrono::duration<double, std::milli>(elapsed).count() / static_cast<double>(kSearches),
        static_cast<double>(stats.expandedNodes) / static_cast<double>(stats.searches),
    };
}

} // namespace

int main() {
    mc::gameplay::entities::registerBuiltinEntities();
    const auto world = makeFlatWorld(3);
    const Measurement baseline = runScenario(world, baselineType());
    const Measurement pursuit = runScenario(world, mc::gameplay::entities::builtinSpecies("zombie"));
    const SearchStressMeasurement stress = runBoundedSearchStress();
    const double aiCost = pursuit.millisecondsPerTick - baseline.millisecondsPerTick;
    const double ratio = baseline.millisecondsPerTick > 0.0
                             ? pursuit.millisecondsPerTick / baseline.millisecondsPerTick
                             : 0.0;

    std::cout << "70 mobs, empty Brain: " << baseline.millisecondsPerTick << " ms/tick\n"
              << "70 zombies pursuing: " << pursuit.millisecondsPerTick << " ms/tick\n"
              << "AI pursuit delta:     " << aiCost << " ms/tick (" << ratio << "x total)\n"
              << "path searches/tick:  "
              << static_cast<double>(pursuit.searches) / static_cast<double>(pursuit.simulatedTicks)
              << '\n'
              << "nodes/path search:   "
              << (pursuit.searches > 0U ? static_cast<double>(pursuit.expandedNodes) /
                                              static_cast<double>(pursuit.searches)
                                        : 0.0)
              << '\n'
              << "blocked search max:   " << stress.millisecondsPerSearch << " ms/search, "
              << stress.expandedNodesPerSearch << " nodes/search\n"
              << "attacks/checksum:     " << pursuit.attacks << " / "
              << baseline.checksum + pursuit.checksum << '\n';
    return 0;
}
