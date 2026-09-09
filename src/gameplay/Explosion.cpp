#include "gameplay/Explosion.hpp"

#include "gameplay/Random.hpp"
#include "world/VoxelRaycast.hpp"

#include <algorithm>
#include <unordered_set>

namespace mc::gameplay {

namespace {

// A cell key that fits one 64-bit hash slot, so the dedup set never allocates a
// node per coordinate triple the way a tuple key would.
[[nodiscard]] std::uint64_t packCell(int x, int y, int z) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 38) ^
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) << 19) ^
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(z));
}

} // namespace

std::vector<world::BlockPos> explodedPositions(const world::World& world,
                                               const ExplosionSpec& spec,
                                               std::uint64_t& randomState) {
    using explosion_detail::kShell;
    using explosion_detail::kStep;
    using explosion_detail::kStepCost;

    std::vector<world::BlockPos> result;
    if (!spec.destroysBlocks || spec.radius <= 0.0F) {
        return result;
    }
    std::unordered_set<std::uint64_t> seen;
    // A blast of radius 4 breaks on the order of a hundred cells; reserving the
    // ray count would be an order of magnitude too much.
    result.reserve(256U);
    seen.reserve(512U);

    for (int xx = 0; xx < kShell; ++xx) {
        for (int yy = 0; yy < kShell; ++yy) {
            for (int zz = 0; zz < kShell; ++zz) {
                // Only the shell of the cube casts a ray.
                const bool onShell = xx == 0 || xx == kShell - 1 || yy == 0 ||
                                     yy == kShell - 1 || zz == 0 || zz == kShell - 1;
                if (!onShell) {
                    continue;
                }
                double dx = static_cast<double>(xx) / 15.0 * 2.0 - 1.0;
                double dy = static_cast<double>(yy) / 15.0 * 2.0 - 1.0;
                double dz = static_cast<double>(zz) / 15.0 * 2.0 - 1.0;
                const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (length <= 0.0) {
                    continue;
                }
                dx /= length;
                dy /= length;
                dz /= length;
                // The per-ray jitter is what gives an explosion its ragged edge;
                // without it every blast is a smooth ball.
                float remaining = spec.radius * (0.7F + mc::rng::nextFloat(randomState) * 0.6F);
                double x = static_cast<double>(spec.center.x);
                double y = static_cast<double>(spec.center.y);
                double z = static_cast<double>(spec.center.z);
                for (; remaining > 0.0F; remaining -= kStepCost) {
                    const int cellX = static_cast<int>(std::floor(x));
                    const int cellY = static_cast<int>(std::floor(y));
                    const int cellZ = static_cast<int>(std::floor(z));
                    if (!world::isWorldYInRange(cellY)) {
                        break;
                    }
                    // vanilla: `getBlockExplosionResistance` returns empty ONLY for
                    // air-with-no-fluid; every other block pays, including the
                    // ones whose resistance is zero (grass, flowers, torches).
                    // Keying on "resistance > 0" instead let a ray cross a
                    // flower bed or a torch for free, which is exactly the
                    // "spreads too far sideways" this had: the ground floor of a
                    // world is full of zero-resistance decoration.
                    const auto cellBlock = world.block(cellX, cellY, cellZ);
                    if (cellBlock != world::Block::Air) {
                        remaining -=
                            (world::blockDefinition(cellBlock).blastResistance + 0.3F) * 0.3F;
                    }
                    if (remaining > 0.0F && cellBlock != world::Block::Air) {
                        if (seen.insert(packCell(cellX, cellY, cellZ)).second) {
                            result.push_back({cellX, cellY, cellZ});
                        }
                    }
                    x += dx * static_cast<double>(kStep);
                    y += dy * static_cast<double>(kStep);
                    z += dz * static_cast<double>(kStep);
                }
            }
        }
    }
    return result;
}

float seenPercent(const world::World& world, glm::vec3 center, glm::vec3 boxMin,
                  glm::vec3 boxMax) {
    const float width = boxMax.x - boxMin.x;
    const float height = boxMax.y - boxMin.y;
    const float depth = boxMax.z - boxMin.z;
    if (width < 0.0F || height < 0.0F || depth < 0.0F) {
        return 0.0F;
    }
    const double xStep = 1.0 / (static_cast<double>(width) * 2.0 + 1.0);
    const double yStep = 1.0 / (static_cast<double>(height) * 2.0 + 1.0);
    const double zStep = 1.0 / (static_cast<double>(depth) * 2.0 + 1.0);
    const double xOffset = (1.0 - std::floor(1.0 / xStep) * xStep) / 2.0;
    const double zOffset = (1.0 - std::floor(1.0 / zStep) * zStep) / 2.0;

    int hits = 0;
    int total = 0;
    for (double u = 0.0; u <= 1.0; u += xStep) {
        for (double v = 0.0; v <= 1.0; v += yStep) {
            for (double w = 0.0; w <= 1.0; w += zStep) {
                const glm::vec3 from{
                    static_cast<float>(static_cast<double>(boxMin.x) +
                                       static_cast<double>(width) * u + xOffset),
                    static_cast<float>(static_cast<double>(boxMin.y) +
                                       static_cast<double>(height) * v),
                    static_cast<float>(static_cast<double>(boxMin.z) +
                                       static_cast<double>(depth) * w + zOffset)};
                // A clear line of sight is a MISS in vanilla's clip() terms: the
                // ray reached the centre without a collider stopping it. The
                // pick-ray walker is the same DDA vanilla's clip uses, so the
                // answer agrees with what a player can see.
                const glm::vec3 toCentre = center - from;
                const float distance = std::sqrt(glm::dot(toCentre, toCentre));
                const bool blocked =
                    distance > 1.0e-4F &&
                    world::raycastVoxels(world, from, toCentre / distance, distance).has_value();
                if (!blocked) {
                    ++hits;
                }
                ++total;
            }
        }
    }
    return total == 0 ? 0.0F : static_cast<float>(hits) / static_cast<float>(total);
}

} // namespace mc::gameplay
