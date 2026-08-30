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

// 相机周围雨域的水平半宽，单位是格
// 旧的正负 16 只要出去几个区块就既没有雨滴也没有水花，近处看不到溅射就是这么来的
// 正负 24 把可见范围拉远了约一倍，而缓存下来的碰撞让这个范围依然便宜
constexpr float kSpawnHalfWidth = 24.0F;
// 相机视点之上的竖直生成窗口
// 天花板 kSpawnMax 同时是探测基准，列扫描从这里往下找最高的碰撞面
// 因此相机上方约 24 格以内的屋顶能挡住雨，而不是让雨直接穿过去
// 生成高度在整个窗口里随机铺开而不是固定一个高度，雨才是连续下落的
// 固定生成高度会让所有雨滴排成一整幅同步下坠的帘子
constexpr float kSpawnMin = 12.0F;
constexpr float kSpawnMax = 32.0F;
// 雨滴至少要出现在它要落向的那个面之上这么远的地方
// 屋顶上的雨因此总能看到一段下落再溅射，而不是凭空出现在屋顶上
constexpr float kDropClearance = 8.0F;
constexpr float kFallSpeed = 18.0F;
// 列探测从天花板往下最多扫这么多行就放弃，对应深空以及相机远高于地面的情形
// 放弃之后雨滴自由落体，落到相机下方就重生，和飞出盒子的雨滴走同一条路
constexpr int kProbeSpan = 96;
// 缓存的面超过这个时长就重新探测，方块编辑与相机升过的屋顶因此最终都能反映出来
// 这个间隔取得很长而不是几秒，因为每隔几秒重探所有列会把碰撞省下的开销还回去大半
// 相机移动本来就会在进入新列时探测，而方块编辑本身很少见
constexpr float kSurfaceProbeInterval = 30.0F;
// 列缓存超出这个容量就整表清空，相机在新地形上流送时它才不会无限增长
constexpr std::size_t kSurfaceCacheLimit = 4096U;
// 墙面缓存超出这个容量就整表清空
// 一面墙每被雨滴穿过一个高度就多一条记录，长墙因此会不断累积
constexpr std::size_t kWallCacheLimit = 2048U;

} // namespace

float RainSystem::randomUnit() {
    // xorshift32 足够便宜，不带 STL 的随机数状态，和渲染器里其它逐帧随机源一样用常量播种
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
    // 盒子是正负 24 格，高度按 16 位放得下，因此水平两轴各 24 位加高度 16 位即可无冲突打包
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
    // 探测天花板及其以下最高的碰撞方块或流体
    // 只有第一滴进入这一列的雨付出这个代价，其余雨滴由缓存供给
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
            // 探测放到相机上方 32 格，能发现高于被接受的 10 格撞击窗口的屋顶
            // 这样那一列会被直接排除，而不是错误地在屋顶下方的室内地面上溅射
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
    // 挑一列能让雨滴生成在其面之上的列
    // 若某列被高于生成窗口的屋顶盖住，雨滴就会出现在室内，所以先换几列重试再退回自由落体
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
                continue; // 被高于生成窗口的屋顶盖住
            }
            // 在刚好高于该面到窗口顶部之间随机取一个高度
            // 随机铺开雨才是连续下落的，否则整片会读成一幅同步下坠的帘子
            const float lowerBound =
                std::max(cameraPosition.y + kSpawnMin, surface.surfaceY + kDropClearance);
            drop.targetSurface = surface.surfaceY;
            drop.position = {x, lowerBound + randomUnit() * (ceiling - lowerBound), z};
        } else {
            // 探测范围内什么都没找到，对应深空，自由落体直到落到相机下方重生
            drop.targetSurface = -1.0F;
            drop.position = {
                x, cameraPosition.y + kSpawnMin + randomUnit() * (kSpawnMax - kSpawnMin), z};
        }
        drop.size = 0.03F + randomUnit() * 0.02F;
        // 每滴各带一点自己的风偏移，斜度因此像一片雨而不是一组刚性平行线
        drop.windJitter = {(randomUnit() - 0.5F) * 0.6F, (randomUnit() - 0.5F) * 0.6F};
        return;
    }
    // 附近每一列都被遮住，也就是玩家在室内，此时自由落体而不是让雨出现在建筑里
    // 位置仍然在盒子里随机取
    // 固定在相机那一点生成会让所有回退的雨滴堆在同一处，看上去像玩家身上插了一根水柱
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
    // 雨下得很快，每一帧雨滴的位置推进 fall 这么多
    const float fall = kFallSpeed * deltaSeconds;
    if (!useCollisionCache_) {
        // 逐滴逐帧的直接碰撞，是最初的全保真路径，每滴每帧一次世界查询
        // 每滴都探测自己所在位置的方块，屋顶、墙面与水面因此都判得精确
        // 代价正是列表面缓存要省掉的那一份
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
                // 撞固体时水珠沿墙弹回来，撞水面时四散喷出
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
        // 缓存碰撞，第一滴进入某列的雨探测一次它的面
        // 其余雨滴只用一次廉价的浮点比较落到缓存值上
        // 风把雨滴吹进新的一列时再读一次缓存
        const float ceiling = cameraPosition.y + kSpawnMax;
        for (auto& drop : drops_) {
            // 侧风让整片雨顺风漂移，而每滴仍保留自己的抖动，雨因此是斜着下而不是直上直下
            drop.position.x += (wind.x + drop.windJitter.x) * deltaSeconds;
            drop.position.z += (wind.y + drop.windJitter.y) * deltaSeconds;
            drop.position.y -= fall;
            const int blockX = static_cast<int>(std::floor(drop.position.x));
            const int blockY = static_cast<int>(std::floor(drop.position.y));
            const int blockZ = static_cast<int>(std::floor(drop.position.z));
            if (blockX != drop.columnX || blockZ != drop.columnZ) {
                drop.columnX = blockX;
                drop.columnZ = blockZ;
                // 风把下落中的雨滴横向带走，它因此可能在自己的高度上撞到墙
                // 墙面方向在第一滴穿过该面时缓存下来
                // 之后到达同一面的雨滴只读缓存并照同一个固定方向溅射，不再重新探测世界
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
                        // 飘进水里的雨滴在水面上四散溅射，和其它落水情形一致
                        splashes_.push_back(RainSplash{
                            {drop.position.x, static_cast<float>(blockY) + 1.0F, drop.position.z},
                            true,
                            glm::vec2{0.0F}});
                    } else {
                        // 背离墙面的方向就是把雨滴带过来的漂移的反方向，水珠因此往回弹
                        // 顺便缓存下来给后面的雨滴用
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
                // 重新取新那一列的面，在第一滴探测过之后这只是一次缓存读取
                // 飘到新出现的屋顶下方的雨滴现在算在室内，安静地让它重生
                // vanilla 的雨粒子进入遮蔽时也是直接消亡
                const ColumnSurface surface = columnSurface(world, blockX, blockZ, ceiling);
                if (surface.surfaceY >= 0.0F && surface.surfaceY > drop.position.y) {
                    respawnDrop(drop, cameraPosition, world);
                    continue;
                }
                drop.targetSurface = surface.surfaceY;
                drop.surfaceWater = surface.water;
            }
            if (drop.targetSurface >= 0.0F && drop.position.y <= drop.targetSurface) {
                // vanilla 的 RainParticle 在它奔向的那个面上消亡，水花从方块顶面发出
                // 地面落点只有随机一小部分真的产出可见水花，连续降雨才不会把粒子表撑满
                // 水面是更显眼的落点，因此每次都溅射
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
