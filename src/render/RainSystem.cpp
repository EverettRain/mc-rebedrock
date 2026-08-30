#include "render/RainSystem.hpp"

#include "gameplay/Random.hpp"
#include "world/Block.hpp"
#include "world/BlockShape.hpp"
#include "world/World.hpp"
#include "world/WorldConstants.hpp"

#include <algorithm>
#include <cmath>

namespace mc::render {
namespace {

// The horizontal half-width of the rain box around the camera, in blocks. The
// old field (±16) left drops and splashes absent just a few chunks out, which
// is why splashes were invisible at a short distance; ±24 keeps the field
// visible roughly twice as far while the cached collision keeps it cheap.
constexpr float kSpawnHalfWidth = 24.0F;
// The vertical spawn window above the camera eye. The ceiling (kSpawnMax) is
// also the probe reference: the column scan starts there and finds the topmost
// collision below it, so a roof up to ~24 blocks above the camera stops the
// rain instead of passing it through. Spawn heights are spread at random across
// the whole window (never a fixed height), so the field is a continuous fall —
// a constant spawn height made every drop fall in one synchronized sheet.
constexpr float kSpawnMin = 12.0F;
constexpr float kSpawnMax = 32.0F;
// A drop materialises at least this far above the surface it is falling onto,
// so rain above a roof always has a visible fall before the splash instead of
// popping into existence on the roof.
constexpr float kDropClearance = 8.0F;
constexpr float kFallSpeed = 18.0F;
// The column probe scans down from the ceiling at most this far before giving
// up (deep void / camera far above the ground): the drop then free-falls and
// respawns below the camera, the same as a drop that falls out of the box.
constexpr int kProbeSpan = 96;
// A cached surface older than this is re-probed, so a block edit or a roof the
// camera rose past eventually shows up. The interval is long (not a couple of
// seconds) because re-probing every column every couple of seconds would
// refund most of the collision win; camera movement already probes new columns
// on entry, and edits are rare.
constexpr float kSurfaceProbeInterval = 30.0F;
// The column cache clears when it outgrows this, keeping it bounded as the
// camera streams across new terrain.
constexpr std::size_t kSurfaceCacheLimit = 4096U;
// The wall-face cache clears when it outgrows this (a wall column caches one
// entry per height a drop crossed it at, so a long wall can accumulate).
constexpr std::size_t kWallCacheLimit = 2048U;

} // namespace

float RainSystem::randomUnit() {
    // xorshift32 — cheap, no STL RNG state, seeded from a constant like the
    // rest of the renderer's per-frame RNGs.
    std::uint32_t value = randomState_;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    randomState_ = value;
    return static_cast<float>(value & 0xFFFFU) / 65535.0F;
}

std::uint64_t RainSystem::columnKey(int blockX, int blockZ) {
    const auto low = static_cast<std::uint32_t>(blockX);
    const auto high = static_cast<std::uint32_t>(blockZ);
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

std::uint64_t RainSystem::wallKey(int blockX, int blockZ, int blockY) {
    // The box is ±24 and heights are 0..255, so 24 bits for each axis and 16
    // for the height pack uniquely without colliding.
    return (static_cast<std::uint64_t>(blockX) & 0xFFFFFFU) |
           ((static_cast<std::uint64_t>(blockZ) & 0xFFFFFFU) << 24) |
           (static_cast<std::uint64_t>(blockY) << 48);
}

RainSystem::ColumnSurface RainSystem::columnSurface(const world::World& world, int blockX,
                                                    int blockZ, float ceiling) {
    const std::uint64_t key = columnKey(blockX, blockZ);
    const auto found = surfaceCache_.find(key);
    if (found != surfaceCache_.end() &&
        timeSeconds_ - found->second.probedAt < kSurfaceProbeInterval) {
        return found->second;
    }
    // Probe: the topmost collision or fluid block at-or-below the ceiling. Only
    // the first drop to enter a column pays this; the cache serves the rest.
    ColumnSurface surface;
    // kWorldHeight 是行数不是坐标上界，世界底也不是 0：两处都必须用 kMaxY/kMinY
    // 写成 kWorldHeight-1 会让探测在世界顶上空扫 64 行，写成 0 则整段深板岩层永远探测不到
    const int top = std::min(static_cast<int>(ceiling), world::kMaxY - 1);
    const int bottom = std::max(top - kProbeSpan, world::kMinY);
    for (int y = top; y >= bottom; --y) {
        ++lastUpdateLookups_;
        const world::Block block = world.block(blockX, y, blockZ);
        if (world::isFluid(block) || world::hasCollision(block)) {
            surface.surfaceY = static_cast<float>(y + 1);
            surface.water = world::isFluid(block);
            break;
        }
    }
    surface.probedAt = timeSeconds_;
    surfaceCache_[key] = surface;
    if (surfaceCache_.size() > kSurfaceCacheLimit) {
        surfaceCache_.clear();
    }
    return surface;
}

float RainSystem::precipitationSurfaceY(const world::World& world, int blockX, int blockZ,
                                        float ceiling) {
    return columnSurface(world, blockX, blockZ, ceiling).surfaceY;
}

void RainSystem::emitTextureImpacts(float deltaSeconds, const glm::vec3& cameraPosition,
                                    float intensity, const world::World& world) {
    constexpr float kWeatherTickSeconds = 1.0F / 20.0F;
    textureImpactAccumulator_ += std::max(deltaSeconds, 0.0F);
    const glm::ivec3 cameraBlock{
        static_cast<int>(std::floor(cameraPosition.x)),
        static_cast<int>(std::floor(cameraPosition.y)),
        static_cast<int>(std::floor(cameraPosition.z)),
    };
    while (textureImpactAccumulator_ >= kWeatherTickSeconds) {
        textureImpactAccumulator_ -= kWeatherTickSeconds;
        ++textureImpactTick_;
        const float gradient = std::clamp(intensity, 0.0F, 1.0F);
        const int sampleCount = static_cast<int>(100.0F * gradient * gradient);
        // 采样序列用共享的 Java LCG（mc::rng），而不是本文件私有的第三份实现
        // 它存在的理由本就是"复刻 vanilla tickRainSplashing 的落点分布"，属于序列奇偶性
        // 诉求而非任意装饰性随机，因此正该走那份权威实现
        // 旧的私有拷贝还漏了 Java nextInt 的 2 的幂快路径，换个 bound 就会静默偏离
        std::uint64_t random = rng::seedFromValue(textureImpactTick_ * 312987231ULL);
        for (int sample = 0; sample < sampleCount; ++sample) {
            const int blockX = cameraBlock.x + static_cast<int>(rng::nextInt(random, 21U)) - 10;
            const int blockZ = cameraBlock.z + static_cast<int>(rng::nextInt(random, 21U)) - 10;
            // A +32 probe finds a roof above the accepted +10 impact window;
            // that column is then rejected instead of incorrectly splashing on
            // an indoor floor beneath the roof.
            const ColumnSurface surface =
                columnSurface(world, blockX, blockZ, cameraPosition.y + kSpawnMax);
            if (surface.surfaceY <= 1.0F ||
                surface.surfaceY > static_cast<float>(cameraBlock.y + 11) ||
                surface.surfaceY < static_cast<float>(cameraBlock.y - 9)) {
                continue;
            }
            const int blockY = static_cast<int>(std::floor(surface.surfaceY)) - 1;
            const float localX = static_cast<float>(rng::nextDouble(random));
            const float localZ = static_cast<float>(rng::nextDouble(random));
            float localSurfaceHeight = 1.0F;
            if (surface.water) {
                // 流体的水面高度由 level 决定，那是流体状态而不是方块形状
                const std::uint8_t level = world.fluidLevel(blockX, blockY, blockZ);
                localSurfaceHeight = level >= 8U
                    ? 1.0F
                    : static_cast<float>(8U - level) / 9.0F;
            } else {
                // 固体表面高度一律问唯一的形状源 world::blockShape
                // 这里曾手写 farmland 与箱子两条分支，箱子还写死成 14/16——那正是
                // BlockShape 里 kChestBox.maxY 的第三份手抄拷贝
                // 手写链同时漏掉了台阶、楼梯、压力板等一切非满方块，水花因此浮在它们上方
                const world::BlockCollisionSpan span =
                    world::verticalSpanOf(world::blockShape(world.state(blockX, blockY, blockZ)));
                localSurfaceHeight = span.top > 0.0F ? span.top : 1.0F;
            }
            splashes_.push_back(RainSplash{
                {static_cast<float>(blockX) + localX,
                 static_cast<float>(blockY) + localSurfaceHeight,
                 static_cast<float>(blockZ) + localZ},
                surface.water,
                {0.0F, 0.0F},
                true,
            });
        }
    }
}

void RainSystem::respawnDrop(RainDrop& drop, const glm::vec3& cameraPosition,
                             const world::World& world) {
    const float ceiling = cameraPosition.y + kSpawnMax;
    // Pick a column whose surface the drop can spawn above. A column covered by
    // a roof higher than the spawn window would materialise the drop indoors,
    // so retry a few columns before falling back to free fall.
    for (int attempt = 0; attempt < 4; ++attempt) {
        const float x = cameraPosition.x + (randomUnit() - 0.5F) * (2.0F * kSpawnHalfWidth);
        const float z = cameraPosition.z + (randomUnit() - 0.5F) * (2.0F * kSpawnHalfWidth);
        const int blockX = static_cast<int>(std::floor(x));
        const int blockZ = static_cast<int>(std::floor(z));
        const ColumnSurface surface = columnSurface(world, blockX, blockZ, ceiling);
        drop.columnX = blockX;
        drop.columnZ = blockZ;
        drop.surfaceWater = surface.water;
        if (surface.surfaceY >= 0.0F) {
            if (surface.surfaceY + kDropClearance > ceiling) {
                continue; // covered by a roof taller than the spawn window
            }
            // Spawn anywhere between just above the surface and the window top,
            // spread at random so the field reads as continuous rain instead of
            // a synchronized sheet.
            const float lowerBound =
                std::max(cameraPosition.y + kSpawnMin, surface.surfaceY + kDropClearance);
            drop.targetSurface = surface.surfaceY;
            drop.position = {x, lowerBound + randomUnit() * (ceiling - lowerBound), z};
        } else {
            // Nothing found in the probe span (deep void): free fall until the
            // drop respawns below the camera.
            drop.targetSurface = -1.0F;
            drop.position = {
                x, cameraPosition.y + kSpawnMin + randomUnit() * (kSpawnMax - kSpawnMin), z};
        }
        drop.size = 0.03F + randomUnit() * 0.02F;
        // Each drop carries its own slight wind offset so the slant reads as a
        // sheet of rain rather than a rigid parallel set.
        drop.windJitter = {(randomUnit() - 0.5F) * 0.6F, (randomUnit() - 0.5F) * 0.6F};
        return;
    }
    // Every nearby column is under cover (the player is indoors): free fall
    // rather than rain materialising inside a building. The position is still
    // random in the box — a fixed camera-point spawn made every fallback drop
    // pile up in one spot and read as a solid water column at the player.
    drop.targetSurface = -1.0F;
    drop.surfaceWater = false;
    drop.position = {
        cameraPosition.x + (randomUnit() - 0.5F) * (2.0F * kSpawnHalfWidth),
        cameraPosition.y + kSpawnMin + randomUnit() * (kSpawnMax - kSpawnMin),
        cameraPosition.z + (randomUnit() - 0.5F) * (2.0F * kSpawnHalfWidth),
    };
    drop.size = 0.03F + randomUnit() * 0.02F;
    drop.windJitter = {(randomUnit() - 0.5F) * 0.6F, (randomUnit() - 0.5F) * 0.6F};
}

void RainSystem::respawnDropFree(RainDrop& drop, const glm::vec3& cameraPosition) {
    drop.position = {
        cameraPosition.x + (randomUnit() - 0.5F) * (2.0F * kSpawnHalfWidth),
        cameraPosition.y + kSpawnMin + randomUnit() * (kSpawnMax - kSpawnMin),
        cameraPosition.z + (randomUnit() - 0.5F) * (2.0F * kSpawnHalfWidth),
    };
    drop.size = 0.03F + randomUnit() * 0.02F;
    drop.windJitter = {(randomUnit() - 0.5F) * 0.6F, (randomUnit() - 0.5F) * 0.6F};
}

void RainSystem::update(float deltaSeconds, const glm::vec3& cameraPosition, float intensity,
                        std::size_t targetCount, const world::World& world, const glm::vec2& wind) {
    timeSeconds_ += deltaSeconds;
    lastUpdateLookups_ = 0U;
    const std::size_t desired =
        intensity <= 0.02F
            ? 0U
            : static_cast<std::size_t>(static_cast<float>(targetCount) * std::min(intensity, 1.0F));
    while (drops_.size() < desired) {
        RainDrop drop;
        respawnDrop(drop, cameraPosition, world);
        drops_.push_back(drop);
    }
    if (drops_.size() > desired) {
        drops_.resize(desired);
    }
    splashes_.clear();
    // Rain falls fast; a drop's position advances by `fall` each frame.
    const float fall = kFallSpeed * deltaSeconds;
    if (!useCollisionCache_) {
        // Direct per-drop-per-frame collision — the original full-fidelity
        // path, one world lookup per drop per frame. Every drop probes the
        // block at its own position, so it catches roofs, walls and water
        // exactly, at the price the cache exists to avoid.
        for (auto& drop : drops_) {
            drop.position.x += (wind.x + drop.windJitter.x) * deltaSeconds;
            drop.position.z += (wind.y + drop.windJitter.y) * deltaSeconds;
            drop.position.y -= fall;
            const int blockX = static_cast<int>(std::floor(drop.position.x));
            const int blockY = static_cast<int>(std::floor(drop.position.y));
            const int blockZ = static_cast<int>(std::floor(drop.position.z));
            ++lastUpdateLookups_;
            const world::Block block = world.block(blockX, blockY, blockZ);
            const bool water = world::isFluid(block);
            if (water || world::hasCollision(block)) {
                // A solid hit sprays back off the wall; a water hit is radial.
                const glm::vec2 drift{wind.x + drop.windJitter.x, wind.y + drop.windJitter.y};
                const float driftLength = std::sqrt(drift.x * drift.x + drift.y * drift.y);
                const glm::vec2 away =
                    (!water && driftLength > 1e-6F) ? -drift / driftLength : glm::vec2{0.0F};
                if (water || randomUnit() < 0.25F) {
                    splashes_.push_back(RainSplash{
                        {drop.position.x, static_cast<float>(blockY) + 1.0F, drop.position.z},
                        water,
                        away});
                }
                respawnDropFree(drop, cameraPosition);
            } else if (drop.position.y < cameraPosition.y - 4.0F) {
                respawnDropFree(drop, cameraPosition);
            }
        }
    } else {
        // Cached collision: the first drop to enter a column probes its surface
        // once; the rest fall to the cached value with a cheap float compare, plus
        // a cache re-read when wind drifts a drop into a new column.
        const float ceiling = cameraPosition.y + kSpawnMax;
        for (auto& drop : drops_) {
            // Cross-wind: the whole field drifts downwind while each drop keeps its
            // own jitter, so the rain falls at an angle instead of straight down.
            drop.position.x += (wind.x + drop.windJitter.x) * deltaSeconds;
            drop.position.z += (wind.y + drop.windJitter.y) * deltaSeconds;
            drop.position.y -= fall;
            const int blockX = static_cast<int>(std::floor(drop.position.x));
            const int blockY = static_cast<int>(std::floor(drop.position.y));
            const int blockZ = static_cast<int>(std::floor(drop.position.z));
            if (blockX != drop.columnX || blockZ != drop.columnZ) {
                drop.columnX = blockX;
                drop.columnZ = blockZ;
                // Wind drift carries the drop sideways as it falls, so it can meet
                // a wall at its own height. The wall-face direction is cached the
                // first time a drop crosses a face; a later drop reaching the same
                // face splashes in that same fixed direction with a cache read
                // instead of re-probing the world.
                const std::uint64_t wall = wallKey(blockX, blockZ, blockY);
                const auto cachedWall = wallCache_.find(wall);
                if (cachedWall != wallCache_.end()) {
                    if (randomUnit() < 0.25F) {
                        splashes_.push_back(RainSplash{
                            {drop.position.x, static_cast<float>(blockY) + 1.0F, drop.position.z},
                            false,
                            cachedWall->second});
                    }
                    respawnDrop(drop, cameraPosition, world);
                    continue;
                }
                const world::Block current = world.block(blockX, blockY, blockZ);
                if (world::hasCollision(current) || world::isFluid(current)) {
                    const bool water = world::isFluid(current);
                    if (water) {
                        // A drop drifting into water splashes radially on the
                        // surface, like any other water landing.
                        splashes_.push_back(RainSplash{
                            {drop.position.x, static_cast<float>(blockY) + 1.0F, drop.position.z},
                            true,
                            glm::vec2{0.0F}});
                    } else {
                        // The direction away from the wall is opposite the drift
                        // that carried the drop into it, so the droplets bounce
                        // back out; cache it for the drops that follow.
                        const glm::vec2 drift{wind.x + drop.windJitter.x,
                                              wind.y + drop.windJitter.y};
                        const float driftLength = std::sqrt(drift.x * drift.x + drift.y * drift.y);
                        const glm::vec2 away =
                            driftLength > 1e-6F ? -drift / driftLength : glm::vec2{-1.0F, 0.0F};
                        wallCache_[wall] = away;
                        if (wallCache_.size() > kWallCacheLimit) {
                            wallCache_.clear();
                        }
                        if (randomUnit() < 0.25F) {
                            splashes_.push_back(
                                RainSplash{{drop.position.x, static_cast<float>(blockY) + 1.0F,
                                            drop.position.z},
                                           false,
                                           away});
                        }
                    }
                    respawnDrop(drop, cameraPosition, world);
                    continue;
                }
                // Re-fetch the new column's surface (a cache read after the first
                // drop there probed it). A drop that drifted under a newly-overhead
                // roof is indoors now — respawn it quietly, the way vanilla rain
                // particles die when they enter cover.
                const ColumnSurface surface = columnSurface(world, blockX, blockZ, ceiling);
                if (surface.surfaceY >= 0.0F && surface.surfaceY > drop.position.y) {
                    respawnDrop(drop, cameraPosition, world);
                    continue;
                }
                drop.targetSurface = surface.surfaceY;
                drop.surfaceWater = surface.water;
            }
            if (drop.targetSurface >= 0.0F && drop.position.y <= drop.targetSurface) {
                // vanilla's RainParticle dies on the surface it is headed for; the
                // splash is emitted at the block's top face. Only a random fraction
                // of ground landings emit a visible splash so the continuous rain
                // never floods the particle list; water, the more visible surface,
                // splashes every time.
                if (drop.surfaceWater || randomUnit() < 0.25F) {
                    splashes_.push_back(
                        RainSplash{{drop.position.x, drop.targetSurface, drop.position.z},
                                   drop.surfaceWater,
                                   glm::vec2{0.0F}});
                }
                respawnDrop(drop, cameraPosition, world);
            } else if (drop.position.y < cameraPosition.y - 4.0F) {
                respawnDrop(drop, cameraPosition, world);
            }
        }
    }
}

} // namespace mc::render
