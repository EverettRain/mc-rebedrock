#include "render/ParticleSystem.hpp"

#include "world/BlockShape.hpp"
#include "world/BlockState.hpp"
#include "world/World.hpp"

#include <algorithm>
#include <cmath>

namespace mc::render {
namespace {

// vanilla ClientLevel#addDestroyBlockEffect 的细分方式
// 轮廓形状的每一个盒子各自按 0.25 切片，每轴至少两片，片心落在盒内而不是摊满整格
//
// 这里曾按 BlockModel 手抄一张轮廓盒表，火把 2x3x2、十字植物 3x4x3、其余一律 4x4x4
// 那是方块形状在仓库里的第六份拷贝，而形状的唯一权威源是 world::blockShape
// 手抄表漏掉了台阶、楼梯、栅栏、门与作物，它们全部按满立方体撒粉尘，粉尘因此飘在空气里
// 它还会随 BlockShape 的修正而静默失准
// 改为直接消费 blockShape 之后，形状一改粒子自动跟上
[[nodiscard]] int piecesAlong(float width) {
    return std::max(2, static_cast<int>(std::ceil(width / 0.25F)));
}

// 遍历形状的盒子，对应 vanilla 的 VoxelShape#forAllBoxes
// Column 展开成填满 1x1 底面的那一个盒子，于是细分只有一条代码路径
// Empty（目前只有 Fire）不产出任何盒子，与 vanilla 一样不撒地形粉尘
template <typename Fn>
void forEachShapeBox(const world::BlockShape& shape, Fn&& callback) {
    switch (shape.kind) {
    case world::ShapeKind::Empty:
        return;
    case world::ShapeKind::Column:
        callback(world::ShapeBox{0.0F, shape.bottom, 0.0F, 1.0F, shape.top, 1.0F});
        return;
    case world::ShapeKind::Boxes:
        for (const world::ShapeBox& box : shape.boxes) {
            callback(box);
        }
        return;
    }
}

} // namespace

float ParticleSystem::randomUnit() {
    randomState_ = randomState_ * 1664525U + 1013904223U;
    return static_cast<float>((randomState_ >> 8U) & 0x00FFFFFFU) /
        static_cast<float>(0x01000000U);
}

void ParticleSystem::setLevelScale(float scale) {
    levelScale_ = std::max(scale, 0.1F);
    // 总上限跟着密度旋钮走，环境天气最多占其中四分之三
    // 剩下那四分之一是留给挖掘、水桶这类即时玩法反馈的硬预留
    particleLimit_ = static_cast<std::size_t>(8000U * levelScale_);
    weatherParticleLimit_ = particleLimit_ * 3U / 4U;

    const std::size_t totalExcess = particles_.size() > particleLimit_
        ? particles_.size() - particleLimit_
        : 0U;
    const std::size_t weatherExcess = weatherParticleCount_ > weatherParticleLimit_
        ? weatherParticleCount_ - weatherParticleLimit_
        : 0U;
    std::size_t weatherToRemove = std::min(
        weatherParticleCount_, std::max(totalExcess, weatherExcess));
    std::erase_if(particles_, [&](const BlockParticle& particle) {
        if (weatherToRemove == 0U || categoryOf(particle.type) != ParticleCategory::Weather) {
            return false;
        }
        --weatherToRemove;
        --weatherParticleCount_;
        return true;
    });
    if (particles_.size() > particleLimit_) {
        const std::size_t remainingExcess = particles_.size() - particleLimit_;
        for (std::size_t index = 0; index < remainingExcess; ++index) {
            if (categoryOf(particles_[index].type) == ParticleCategory::Weather) {
                --weatherParticleCount_;
            }
        }
        particles_.erase(particles_.begin(), particles_.begin() +
            static_cast<std::ptrdiff_t>(remainingExcess));
    }
}

void ParticleSystem::reserveGameplayCapacity(std::size_t requested) {
    const std::size_t available = particleLimit_ > particles_.size()
        ? particleLimit_ - particles_.size()
        : 0U;
    std::size_t weatherToRemove = std::min(
        requested > available ? requested - available : 0U,
        weatherParticleCount_);
    std::erase_if(particles_, [&](const BlockParticle& particle) {
        if (weatherToRemove == 0U || categoryOf(particle.type) != ParticleCategory::Weather) {
            return false;
        }
        --weatherToRemove;
        --weatherParticleCount_;
        return true;
    });
}

bool ParticleSystem::weatherCapacityAvailable() const {
    return particles_.size() < particleLimit_ &&
           weatherParticleCount_ < weatherParticleLimit_;
}

int ParticleSystem::scaledCount(int base) {
    if (base <= 0) {
        return 0;
    }
    const float scaled = static_cast<float>(base) * levelScale_;
    const int whole = static_cast<int>(scaled);
    // 小数部分用一次掷币进位，低档 0.5 这样的系数因此长期期望正好是 base 乘以系数
    // 直接截断会系统性偏低
    return whole + (randomUnit() < scaled - static_cast<float>(whole) ? 1 : 0);
}

bool ParticleSystem::add(const ParticleSpawn& spawn) {
    const ParticleTypeDefinition& definition = particleTypeOf(spawn.type);
    // 预算门在这里收口。从前四个 spawnXxx 各写一遍「玩法看总量、天气再看天气配额」，
    // 四份写法的细节还不完全一致；现在只有这一处
    if (definition.category == ParticleCategory::Weather) {
        if (!weatherCapacityAvailable()) {
            return false;
        }
    } else if (particles_.size() >= particleLimit_) {
        return false;
    }

    BlockParticle particle;
    particle.type = spawn.type;
    particle.size = spawn.size;
    particle.lifetimeSeconds = spawn.lifetimeSeconds;
    if (definition.sprite == kNoParticleSprite) {
        // 纹理由发射侧给：方块粉尘取被挖方块的贴图并在 64x64 里取一块 16x16 子图
        particle.textureLayer = spawn.textureLayer;
        particle.uvOrigin = spawn.uvOrigin;
        particle.uvScale = spawn.uvScale;
    } else {
        // SpriteSet#get(random)：从这一类的精灵集里随机取一张，整层一张图
        const ParticleSpriteSet& set = particleSpriteSet(definition.sprite);
        const auto index = static_cast<std::uint32_t>(
            randomUnit() * static_cast<float>(set.textures.size()));
        particle.textureLayer =
            set.firstLayer +
            static_cast<float>(std::min<std::size_t>(index, set.textures.size() - 1U));
        particle.uvOrigin = {0.0F, 0.0F};
        particle.uvScale = 1.0F;
    }
    // 颜色 = tintBase × U[brightnessMin, brightnessMax]。不着色的类型两端都是 1
    // 且 tintBase 是白，打包出来正好是「白」，不是哨兵 0 —— 白色的乘法本就是恒等，
    // 走同一条公式不必分支
    if (definition.tintBase == glm::vec3{1.0F, 1.0F, 1.0F} &&
        definition.brightnessMin == 1.0F && definition.brightnessMax == 1.0F) {
        particle.tint = kNoParticleTint;
    } else {
        const float brightness =
            definition.brightnessMin +
            randomUnit() * (definition.brightnessMax - definition.brightnessMin);
        particle.tint = packParticleTint(definition.tintBase * brightness);
    }

    switch (definition.motion) {
    case ParticleMotion::Ballistic:
        particle.position = spawn.position;
        particle.velocity = spawn.motion;
        break;
    case ParticleMotion::FlyTowards:
        // vanilla：xStart 是构造时传入的位置（终点），而粒子从 xStart + xd 起飞
        particle.anchor = spawn.position;
        particle.velocity = spawn.motion;
        particle.position = spawn.position + spawn.motion;
        break;
    }

    particles_.push_back(particle);
    if (definition.category == ParticleCategory::Weather) {
        ++weatherParticleCount_;
    }
    return true;
}

void ParticleSystem::spawnBlockBreak(
    const glm::ivec3& blockPosition,
    world::Block block) {
    const float layer = world::textureLayers(block).side;
    // 形状取自唯一权威源 world::blockShape
    // ParticleEvent 目前只携带 Block，所以这里用该方块的默认状态
    // 台阶上下半、楼梯朝向、作物生长阶段这些逐状态差异要等事件一并带上 BlockState 才能精确
    // 在那之前，默认状态的形状已经严格优于原来那张按模型手抄的常数表
    const world::BlockShape shape = world::blockShape(world::BlockState{block});
    // 密度旋钮在这里不走 scaledCount，因为要保住粉尘填满方块体积的几何形态
    // 系数大于一时每个格子生成多份带抖动的粉尘，小于一时按概率整格丢弃
    // 一次 64 粒的破坏在高档与疯狂档变成 128 与 192 粒，低档约 32 粒
    const int copiesPerCell =
        levelScale_ >= 1.0F ? static_cast<int>(levelScale_) : 1;
    const float retainProbability = std::min(levelScale_, 1.0F);

    // 预留的是打折后的期望数量，并且要把形状的每一个盒子都算进去
    // 低档下格子会按 retainProbability 概率整格丢弃，实际只生成大约一半
    // 若仍按满格数预留，每挖一格就会多驱逐约一半数量的天气粒子
    // 表现出来就是低档下雨挖矿时雨会一阵阵变稀
    float cellCount = 0.0F;
    forEachShapeBox(shape, [&](const world::ShapeBox& box) {
        const float widthX = std::min(1.0F, box.maxX - box.minX);
        const float widthY = std::min(1.0F, box.maxY - box.minY);
        const float widthZ = std::min(1.0F, box.maxZ - box.minZ);
        cellCount += static_cast<float>(piecesAlong(widthX) * piecesAlong(widthY) *
                                        piecesAlong(widthZ));
    });
    reserveGameplayCapacity(static_cast<std::size_t>(std::ceil(cellCount * retainProbability)) *
                            static_cast<std::size_t>(copiesPerCell));

    bool poolFull = false;
    forEachShapeBox(shape, [&](const world::ShapeBox& box) {
        if (poolFull) {
            return;
        }
        const float widthX = std::min(1.0F, box.maxX - box.minX);
        const float widthY = std::min(1.0F, box.maxY - box.minY);
        const float widthZ = std::min(1.0F, box.maxZ - box.minZ);
        const int countX = piecesAlong(widthX);
        const int countY = piecesAlong(widthY);
        const int countZ = piecesAlong(widthZ);
        for (int y = 0; y < countY; ++y) {
            for (int z = 0; z < countZ; ++z) {
                for (int x = 0; x < countX; ++x) {
                    if (particles_.size() >= particleLimit_) {
                        poolFull = true;
                        return;
                    }
                    if (randomUnit() >= retainProbability) {
                        continue;
                    }
                    for (int copy = 0; copy < copiesPerCell; ++copy) {
                        if (particles_.size() >= particleLimit_) {
                            poolFull = true;
                            return;
                        }
                        // vanilla 的片心是盒内的归一化偏移 rel，实际位置是 rel 乘以盒宽再加盒下界
                        // 速度方向直接取 rel 减去 0.5，也就是从盒心向外
                        // 原来把 rel 当成整格坐标用，细瘦的火把盒因此把粉尘撒满了整格
                        const glm::vec3 rel{
                            (static_cast<float>(x) + 0.5F) / static_cast<float>(countX),
                            (static_cast<float>(y) + 0.5F) / static_cast<float>(countY),
                            (static_cast<float>(z) + 0.5F) / static_cast<float>(countZ),
                        };
                        const glm::vec3 local{
                            rel.x * widthX + box.minX,
                            rel.y * widthY + box.minY,
                            rel.z * widthZ + box.minZ,
                        };
                        // 抖动幅度取半片，让同一格里的多份拷贝不至于完全重叠
                        // 按片长而不是固定的 0.25 缩放，细瘦的盒子才不会把粉尘甩到盒外
                        const glm::vec3 jitter{
                            (randomUnit() - 0.5F) * widthX / static_cast<float>(countX),
                            (randomUnit() - 0.5F) * widthY / static_cast<float>(countY),
                            (randomUnit() - 0.5F) * widthZ / static_cast<float>(countZ),
                        };
                        glm::vec3 direction = rel - glm::vec3{0.5F};
                        // 对应 vanilla Particle 的速度构造，逐分量抖动正负 0.4 后重新归一化
                        // 再乘以每 tick 0.4 的系数，这里乘 20 把每 tick 换算成每秒
                        // vanilla 每 tick 额外的 0.1 上抬在这里就是每秒 2.0
                        direction += glm::vec3{
                            randomUnit() - 0.5F,
                            randomUnit() - 0.5F,
                            randomUnit() - 0.5F} * 0.8F;
                        const float length =
                            std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                      direction.z * direction.z);
                        const float speed =
                            (randomUnit() + randomUnit() + 1.0F) * 0.15F * 8.0F;
                        const glm::vec3 velocity =
                            direction / std::max(length, 1e-6F) * speed;
                        // BlockDustParticle 在 64x64 的方块纹理里随机取一块 16x16 的子图
                        // uvScale 取 0.25 正好就是这一块子图
                        const glm::vec2 uvOrigin{
                            static_cast<float>(static_cast<int>(randomUnit() * 4.0F)) * 0.25F,
                            static_cast<float>(static_cast<int>(randomUnit() * 4.0F)) * 0.25F,
                        };
                        if (!add(ParticleSpawn{
                                .type = ParticleType::BlockDust,
                                .position = glm::vec3{blockPosition} + local + jitter,
                                .motion = velocity + glm::vec3{0.0F, 2.0F, 0.0F},
                                // SpriteBillboardParticle 的尺寸折半，落在 0.05 到 0.1 之间
                                .size = 0.05F + randomUnit() * 0.05F,
                                // 对应 vanilla Particle 的 maxAge，即 4 除以 rand 乘 0.9 加 0.1 个 tick
                                .lifetimeSeconds = (4.0F / (0.1F + randomUnit() * 0.9F)) / 20.0F,
                                .textureLayer = layer,
                                .uvOrigin = uvOrigin,
                                .uvScale = 0.25F,
                            })) {
                            poolFull = true;
                            return;
                        }
                    }
                }
            }
        }
    });
}

void ParticleSystem::spawnWaterSplash(const glm::vec3& position) {
    constexpr float kFullTurn = 6.28318530718F;
    const int count = scaledCount(10);
    reserveGameplayCapacity(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const float angle = randomUnit() * kFullTurn;
        const float speed = 0.25F + randomUnit() * 0.55F;
        if (!add(ParticleSpawn{
                .type = ParticleType::WaterSplash,
                .position = position,
                .motion = {std::cos(angle) * speed, 0.65F + randomUnit() * 0.75F,
                           std::sin(angle) * speed},
                .size = 0.06F + randomUnit() * 0.035F,
                .lifetimeSeconds = 0.45F + randomUnit() * 0.25F,
                .textureLayer = world::textureLayers(world::Block::Water).side,
                .uvOrigin = {randomUnit() * 0.5F, randomUnit() * 0.5F},
                .uvScale = 0.25F,
            })) {
            return;
        }
    }
}

void ParticleSystem::spawnRainImpact(const glm::vec3& position, bool onWater) {
    // vanilla 的 RainSplashParticle 没有水平速度，向上初速为每 tick 0.1 到 0.3 格
    // 寿命是 8 除以 random 乘 0.8 加 0.2 个 tick，这里一律换算成秒与每秒格数
    // 逐撞击的密度倍率保留，粒子等级调高时雨柱与地面反应会同时变密
    for (int index = 0; index < scaledCount(1); ++index) {
        const float lifetime = (8.0F / (0.2F + randomUnit() * 0.8F)) / 20.0F;
        const float surfaceScale = onWater ? 1.15F : 1.0F;
        if (!add(ParticleSpawn{
                .type = ParticleType::RainImpact,
                .position = position,
                .motion = {0.0F, 2.0F + randomUnit() * 4.0F, 0.0F},
                .size = (0.075F + randomUnit() * 0.025F) * surfaceScale,
                .lifetimeSeconds = lifetime,
                .textureLayer = world::textureLayers(world::Block::Water).side,
                .uvOrigin = {randomUnit() * 0.5F, randomUnit() * 0.5F},
                .uvScale = 0.25F,
            })) {
            return;
        }
    }
}

void ParticleSystem::spawnRainSplash(const glm::vec3& position, const glm::vec2& direction) {
    // 比水桶那一下的爆发更少也更短命，一滴雨落地只推出几颗很快消散的水珠
    // 尺寸与时长比方块粉尘略大一点，隔着二三十格也还能看出雨确实落在地上
    // direction 非零时表示雨滴飘进的墙面，水珠沿背离墙面的半圆扇形喷出
    // direction 为零时四散喷出
    constexpr float kFullTurn = 6.28318530718F;
    constexpr float kHalfTurn = 3.14159265359F;
    const bool directional = direction.x * direction.x + direction.y * direction.y > 0.25F;
    const float baseAngle = directional ? std::atan2(direction.y, direction.x) : 0.0F;
    // 落地水花自己的密度倍率与雨滴总量的倍率同时生效，放大后的天气预算吃得下这个峰值
    for (int index = 0; index < scaledCount(4); ++index) {
        const float angle = directional
            ? baseAngle - kHalfTurn * 0.5F + randomUnit() * kHalfTurn
            : randomUnit() * kFullTurn;
        const float speed = 0.20F + randomUnit() * 0.40F;
        if (!add(ParticleSpawn{
                .type = ParticleType::RainSplash,
                .position = position + glm::vec3{(randomUnit() - 0.5F) * 0.2F, 0.0F,
                                                 (randomUnit() - 0.5F) * 0.2F},
                .motion = {std::cos(angle) * speed, 0.50F + randomUnit() * 0.50F,
                           std::sin(angle) * speed},
                .size = 0.08F + randomUnit() * 0.04F,
                .lifetimeSeconds = 0.50F + randomUnit() * 0.25F,
                .textureLayer = world::textureLayers(world::Block::Water).side,
                .uvOrigin = {randomUnit() * 0.5F, randomUnit() * 0.5F},
                .uvScale = 0.25F,
            })) {
            return;
        }
    }
}

void ParticleSystem::advanceFlyTowards(BlockParticle& particle) {
    // FlyTowardsPositionParticle#tick，逐字：
    //   f = age/lifetime; pos = 1 - f; pp = (1 - pos)^4 = f^4
    //   x = xStart + xd*pos;  y = yStart + yd*pos - pp*1.2;  z = zStart + zd*pos
    // 位置是寿命的函数而不是速度的积分，所以既不吃重力也不吃阻力：age 归零就回到起点
    const float fraction =
        std::clamp(particle.ageSeconds / std::max(particle.lifetimeSeconds, 1e-6F), 0.0F, 1.0F);
    const float remaining = 1.0F - fraction;
    const float sag = fraction * fraction * fraction * fraction;
    particle.position = particle.anchor + particle.velocity * remaining;
    particle.position.y -= sag * 1.2F;
}

void ParticleSystem::advanceBallistic(BlockParticle& particle, float elapsed, float airDrag,
                                      float groundDrag, const world::World& world) {
    const ParticleTypeDefinition& definition = particleTypeOf(particle.type);
    particle.velocity.y -= 16.0F * definition.gravityScale * elapsed;
    const glm::vec3 next = particle.position + particle.velocity * elapsed;
    if (!definition.collides) {
        // vanilla Particle#hasPhysics == false：穿过方块，也不吃地面阻力
        particle.position = next;
        particle.velocity *= airDrag;
        return;
    }
    const int blockX = static_cast<int>(std::floor(next.x));
    const int blockY = static_cast<int>(std::floor(next.y));
    const int blockZ = static_cast<int>(std::floor(next.z));
    // 读的是状态而不是方块，状态既能给出方块身份也能给出这一格的形状
    // 落地面高度因此是换掉原来那次 world.block 查询，不是在它之上再加一次
    const world::BlockState landedState = world.state(blockX, blockY, blockZ);
    const world::Block landedBlock = landedState.block();
    const bool landedFluid = world::isFluid(landedBlock);
    // 落地面取该格形状的顶面，而不是恒定的格顶
    // 写死 blockY 加一是方块形状在本支线里的第三份手抄拷贝
    // 那样粉尘会停在台阶、压力板与农田上方的半空，甚至被从侧面进入的格子向上弹一截
    // 流体的顶面仍按格顶算，因为流体自身的形状是 Empty
    float surfaceTop = 1.0F;
    if (!landedFluid && world::hasCollision(landedBlock)) {
        const world::BlockCollisionSpan span =
            world::verticalSpanOf(world::blockShape(landedState));
        surfaceTop = span.top > 0.0F ? span.top : 1.0F;
    }
    const float surfaceY = static_cast<float>(blockY) + surfaceTop;
    // 流体表面和固体一样算落地，雨水花因此停在水面上而不是在重力下继续沉下去
    if ((world::hasCollision(landedBlock) || landedFluid) &&
        particle.velocity.y < 0.0F && next.y <= surfaceY) {
        // vanilla 的方块粉尘落地即停不反弹，随后靠地面阻力滑一小段并在原地消亡
        particle.position = {next.x, surfaceY, next.z};
        particle.velocity.y = 0.0F;
        particle.velocity.x *= groundDrag;
        particle.velocity.z *= groundDrag;
    } else {
        particle.position = next;
    }
    particle.velocity *= airDrag;
}

void ParticleSystem::update(float deltaSeconds, const world::World& world) {
    const float elapsed = std::max(deltaSeconds, 0.0F);
    // 对应 vanilla 的 Particle#tick，把固定 20 TPS 的基准换算成连续的帧时间
    // 重力是每 tick 平方 0.04 格，速度按秒存储，因此这里是每秒平方 16 格
    // 空气阻力是每 tick 乘 0.98，落地后水平两轴再额外每 tick 乘 0.7
    const float airDrag = std::pow(0.98F, elapsed * 20.0F);
    const float groundDrag = std::pow(0.7F, elapsed * 20.0F);
    for (auto& particle : particles_) {
        particle.ageSeconds += elapsed;
        const ParticleTypeDefinition& definition = particleTypeOf(particle.type);
        // 整层唯一的运动分派点，对应 vanilla 各 Particle 子类的 tick()
        switch (definition.motion) {
        case ParticleMotion::Ballistic:
            advanceBallistic(particle, elapsed, airDrag, groundDrag, world);
            break;
        case ParticleMotion::FlyTowards:
            advanceFlyTowards(particle);
            break;
        }
        const float lived =
            std::clamp(particle.ageSeconds / std::max(particle.lifetimeSeconds, 1e-6F),
                       0.0F, 1.0F);
        // vanilla 的多数粒子直接消失，本项目的地形/天气粒子一直带一段淡出尾巴；
        // 哪一种由类型说了算，而不是由这里统一决定
        particle.opacity =
            definition.fadesOut ? std::clamp((1.0F - lived) * 1.5F, 0.0F, 1.0F) : 1.0F;
        // 自发光 = (age/lifetime)^emissionPower，0 次幂的类型直接给 0 而不是 1
        particle.emission =
            definition.emissionPower > 0.0F ? std::pow(lived, definition.emissionPower) : 0.0F;
    }
    std::erase_if(particles_, [&](const BlockParticle& particle) {
        if (particle.ageSeconds < particle.lifetimeSeconds) {
            return false;
        }
        if (categoryOf(particle.type) == ParticleCategory::Weather) {
            --weatherParticleCount_;
        }
        return true;
    });
}

} // namespace mc::render
