#include "render/ParticleSystem.hpp"

#include "world/World.hpp"

#include <algorithm>
#include <cmath>

namespace mc::render {
namespace {

// Vanilla ParticleManager#addBlockBreakParticles subdivides the broken block's
// outline shape into 0.25-wide cells and spawns one dust per cell. The per-axis
// counts come from the vanilla outline boxes: a full cube is 4x4x4, the torch's
// 0.25 x 0.625 x 0.25 box is 2x3x2, and a cross plant's 0.75 x 1.0 x 0.75 box
// is 3x4x3.
struct PieceCount final {
    int x;
    int y;
    int z;
};

[[nodiscard]] PieceCount breakPieceCount(world::Block block) {
    switch (world::blockDefinition(block).model) {
    case world::BlockModel::Torch:
        return {2, 3, 2};
    case world::BlockModel::Cross:
        return {3, 4, 3};
    default:
        return {4, 4, 4};
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
    // The total ceiling rides the density knob, while ambient weather owns at
    // most 75%. The remaining quarter is a hard reserve for mining, buckets
    // and other immediate gameplay feedback.
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
        if (weatherToRemove == 0U || particle.category != ParticleCategory::Weather) {
            return false;
        }
        --weatherToRemove;
        --weatherParticleCount_;
        return true;
    });
    if (particles_.size() > particleLimit_) {
        const std::size_t remainingExcess = particles_.size() - particleLimit_;
        for (std::size_t index = 0; index < remainingExcess; ++index) {
            if (particles_[index].category == ParticleCategory::Weather) {
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
        if (weatherToRemove == 0U || particle.category != ParticleCategory::Weather) {
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
    // The fractional remainder: a level like 0.5 carries a coin flip so the
    // long-run count is exactly base * scale, never a biased floor.
    return whole + (randomUnit() < scaled - static_cast<float>(whole) ? 1 : 0);
}

void ParticleSystem::spawnBlockBreak(
    const glm::ivec3& blockPosition,
    world::Block block) {
    const float layer = world::textureLayers(block).side;
    const auto counts = breakPieceCount(block);
    // The 粒子效果 density knob: above 1.0 every cell spawns `copies` jittered
    // dust particles, below 1.0 cells drop out probabilistically — the
    // block-filling geometry survives while the long-run count tracks the
    // level (a 64-particle break becomes 128/192 at 高/疯狂, 32 at 低).
    const int copiesPerCell =
        levelScale_ >= 1.0F ? static_cast<int>(levelScale_) : 1;
    const float retainProbability = std::min(levelScale_, 1.0F);
    reserveGameplayCapacity(
        static_cast<std::size_t>(counts.x * counts.y * counts.z * copiesPerCell));
    for (int y = 0; y < counts.y; ++y) {
        for (int z = 0; z < counts.z; ++z) {
            for (int x = 0; x < counts.x; ++x) {
                if (particles_.size() >= particleLimit_) {
                    return;
                }
                if (randomUnit() >= retainProbability) {
                    continue;
                }
                for (int copy = 0; copy < copiesPerCell; ++copy) {
                    if (particles_.size() >= particleLimit_) {
                        return;
                    }
                    // Each dust particle starts at its cell centre and flies
                    // away from the block centre (vanilla's direction is the
                    // cell offset minus half a block); the extra copies share
                    // the cell but jitter its origin so they do not stack.
                    const glm::vec3 origin{
                        (static_cast<float>(x) + 0.5F) / static_cast<float>(counts.x),
                        (static_cast<float>(y) + 0.5F) / static_cast<float>(counts.y),
                        (static_cast<float>(z) + 0.5F) / static_cast<float>(counts.z),
                    };
                    const glm::vec3 jitter{(randomUnit() - 0.5F) * 0.25F,
                                           (randomUnit() - 0.5F) * 0.25F,
                                           (randomUnit() - 0.5F) * 0.25F};
                    glm::vec3 direction = origin + jitter - glm::vec3{0.5F};
                    // Particle's velocity constructor: jitter each component by
                    // +-0.4, renormalise, then scale by f * 0.4 per tick — the
                    // factor of 20 converts blocks-per-tick to blocks-per-second,
                    // and the +0.1/tick lift is +2.0/s.
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
                    // BlockDustParticle samples a random 16x16 sub-tile of the
                    // 64x64 block texture; uvScale 0.25 is exactly that sub-tile.
                    const glm::vec2 uvOrigin{
                        static_cast<float>(static_cast<int>(randomUnit() * 4.0F)) * 0.25F,
                        static_cast<float>(static_cast<int>(randomUnit() * 4.0F)) * 0.25F,
                    };
                    particles_.push_back({
                        glm::vec3{blockPosition} + origin + jitter,
                        velocity + glm::vec3{0.0F, 2.0F, 0.0F},
                        layer,
                        uvOrigin,
                        0.25F,
                        // SpriteBillboardParticle's scale divided by two: 0.05..0.1.
                        0.05F + randomUnit() * 0.05F,
                        0.0F,
                        // Particle's maxAge: 4 / (rand*0.9 + 0.1) ticks.
                        (4.0F / (0.1F + randomUnit() * 0.9F)) / 20.0F,
                        1.0F,
                    });
                }
            }
        }
    }
}

void ParticleSystem::spawnWaterSplash(const glm::vec3& position) {
    constexpr float kFullTurn = 6.28318530718F;
    const int count = scaledCount(10);
    reserveGameplayCapacity(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        if (particles_.size() >= particleLimit_) {
            return;
        }
        const float angle = randomUnit() * kFullTurn;
        const float speed = 0.25F + randomUnit() * 0.55F;
        particles_.push_back({
            position,
            {std::cos(angle) * speed,
             0.65F + randomUnit() * 0.75F,
             std::sin(angle) * speed},
            world::textureLayers(world::Block::Water).side,
            {randomUnit() * 0.5F, randomUnit() * 0.5F},
            0.25F,
            0.06F + randomUnit() * 0.035F,
            0.0F,
            0.45F + randomUnit() * 0.25F,
            0.8F,
        });
    }
}

void ParticleSystem::spawnRainImpact(const glm::vec3& position, bool onWater) {
    // RainSplashParticle starts with no horizontal motion, an upward velocity
    // of 0.1..0.3 blocks/tick, and lives for 8/(random*0.8+0.2) ticks. Convert
    // those values to this particle system's seconds/blocks-per-second units.
    // Keep the per-impact density multiplier: higher particle levels increase
    // both the rain columns and the visible surface response.
    for (int index = 0; index < scaledCount(1); ++index) {
        if (!weatherCapacityAvailable()) {
            return;
        }
        const float lifetime = (8.0F / (0.2F + randomUnit() * 0.8F)) / 20.0F;
        const float surfaceScale = onWater ? 1.15F : 1.0F;
        particles_.push_back({
            position,
            {0.0F, 2.0F + randomUnit() * 4.0F, 0.0F},
            world::textureLayers(world::Block::Water).side,
            {randomUnit() * 0.5F, randomUnit() * 0.5F},
            0.25F,
            (0.075F + randomUnit() * 0.025F) * surfaceScale,
            0.0F,
            lifetime,
            0.82F,
            ParticleCategory::Weather,
        });
        ++weatherParticleCount_;
    }
}

void ParticleSystem::spawnRainSplash(const glm::vec3& position, const glm::vec2& direction) {
    // Fewer, shorter-lived particles than the bucket's burst: a rain drop
    // landing pushes a handful of droplets outward and upward that fade fast.
    // Sized and timed a bit larger than a block-dust puff so the splash reads
    // as the rain lands even a couple dozen blocks out. A non-zero direction
    // (a wall the drop drifted into) sprays the droplets in a half-circle fan
    // facing away from the wall; a zero direction sprays radially.
    constexpr float kFullTurn = 6.28318530718F;
    constexpr float kHalfTurn = 3.14159265359F;
    const bool directional = direction.x * direction.x + direction.y * direction.y > 0.25F;
    const float baseAngle = directional ? std::atan2(direction.y, direction.x) : 0.0F;
    // Preserve the landing-splash density multiplier in addition to the rain
    // population multiplier; the enlarged weather budget absorbs the peak.
    for (int index = 0; index < scaledCount(4); ++index) {
        if (!weatherCapacityAvailable()) {
            return;
        }
        const float angle = directional
            ? baseAngle - kHalfTurn * 0.5F + randomUnit() * kHalfTurn
            : randomUnit() * kFullTurn;
        const float speed = 0.20F + randomUnit() * 0.40F;
        particles_.push_back({
            position + glm::vec3{(randomUnit() - 0.5F) * 0.2F, 0.0F,
                                 (randomUnit() - 0.5F) * 0.2F},
            {std::cos(angle) * speed,
             0.50F + randomUnit() * 0.50F,
             std::sin(angle) * speed},
            world::textureLayers(world::Block::Water).side,
            {randomUnit() * 0.5F, randomUnit() * 0.5F},
            0.25F,
            0.08F + randomUnit() * 0.04F,
            0.0F,
            0.50F + randomUnit() * 0.25F,
            0.7F,
            ParticleCategory::Weather,
        });
        ++weatherParticleCount_;
    }
}

void ParticleSystem::update(float deltaSeconds, const world::World& world) {
    const float elapsed = std::max(deltaSeconds, 0.0F);
    // Particle#tick, converted from the fixed 20 TPS base to a continuous
    // frame time: gravity is 0.04 blocks/tick^2 (0.04 * 20^2 = 16 blocks/s^2;
    // velocity is stored per-second, so the per-tick decrement of 0.04 blocks/tick
    // is a 16-blocks/s-per-second acceleration), air drag is 0.98/tick, and the
    // ground drag a further 0.7/tick on the horizontal axes.
    const float airDrag = std::pow(0.98F, elapsed * 20.0F);
    const float groundDrag = std::pow(0.7F, elapsed * 20.0F);
    for (auto& particle : particles_) {
        particle.ageSeconds += elapsed;
        particle.velocity.y -= 16.0F * elapsed;
        const glm::vec3 next = particle.position + particle.velocity * elapsed;
        const int blockX = static_cast<int>(std::floor(next.x));
        const int blockY = static_cast<int>(std::floor(next.y));
        const int blockZ = static_cast<int>(std::floor(next.z));
        const world::Block landedBlock = world.block(blockX, blockY, blockZ);
        // A fluid surface counts as a landing just like a solid block: a rain
        // splash drops onto the water's top face and sits there, instead of
        // sinking below it under gravity.
        if ((world::hasCollision(landedBlock) || world::isFluid(landedBlock)) &&
            particle.velocity.y < 0.0F) {
            // Vanilla block dust stops dead on the floor instead of bouncing;
            // the ground drag then lets it slide a little and expire in place.
            particle.position = {next.x, static_cast<float>(blockY) + 1.0F, next.z};
            particle.velocity.y = 0.0F;
            particle.velocity.x *= groundDrag;
            particle.velocity.z *= groundDrag;
        } else {
            particle.position = next;
        }
        particle.velocity *= airDrag;
        const float remaining = 1.0F -
            particle.ageSeconds / particle.lifetimeSeconds;
        particle.opacity = std::clamp(remaining * 1.5F, 0.0F, 1.0F);
    }
    std::erase_if(particles_, [&](const BlockParticle& particle) {
        if (particle.ageSeconds < particle.lifetimeSeconds) {
            return false;
        }
        if (particle.category == ParticleCategory::Weather) {
            --weatherParticleCount_;
        }
        return true;
    });
}

} // namespace mc::render
