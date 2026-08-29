#include "world/gen/MultiNoiseBiomeSource.hpp"

#include "world/gen/JavaRandom.hpp"

#include <cstddef>
#include <limits>

namespace mc::world::gen {
namespace {

// The vanilla nether biome parameter points (Biome.MixedNoisePoint). The distance
// a column's climate sits from each point (plus the point's offset) picks the
// biome, so a warp/crimson pocket forms where the humidity noise runs high/low
// and the deltas where the temperature runs cold.
constexpr std::array<MultiNoiseBiomeSource::NoisePoint, 5> kNetherPoints{{
    {Biome::NetherWastes, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
    {Biome::SoulSandValley, 0.0F, -0.5F, 0.0F, 0.0F, 0.0F},
    {Biome::CrimsonForest, 0.4F, 0.0F, 0.0F, 0.0F, 0.0F},
    {Biome::WarpedForest, 0.0F, 0.5F, 0.0F, 0.0F, 0.375F},
    {Biome::BasaltDeltas, -0.5F, 0.0F, 0.0F, 0.0F, 0.175F},
}};

// The octave range vanilla's nether climate noises use: a low-frequency stack so a
// biome spans a broad region rather than flickering cell to cell.
constexpr int kClimateOctaves = 4;

// The distance metric MultiNoiseBiomeSource#Biome.MixedNoisePoint.distanceTo
// uses: squared Euclidean over the four climate axes, plus the offset term
// (added, not squared — a flat rarity bias).
[[nodiscard]] float distanceTo(const MultiNoiseBiomeSource::NoisePoint& point, float temperature,
                               float humidity, float altitude, float weirdness) {
    const float dt = point.temperature - temperature;
    const float dh = point.humidity - humidity;
    const float da = point.altitude - altitude;
    const float dw = point.weirdness - weirdness;
    return dt * dt + dh * dh + da * da + dw * dw + point.offset * point.offset;
}

} // namespace

// Each climate noise gets its own stream, salted off the world seed, so the four
// fields are independent and the whole map is deterministic per seed and distinct
// from the overworld (the seed is already dimensionSeed(worldSeed, Nether) by the
// time it reaches here).
MultiNoiseBiomeSource::MultiNoiseBiomeSource(std::uint64_t seed)
    : temperature_([seed] {
          JavaRandom random{seed};
          return OctavePerlinNoiseSampler{random, kClimateOctaves};
      }()),
      humidity_([seed] {
          JavaRandom random{seed + 1U};
          return OctavePerlinNoiseSampler{random, kClimateOctaves};
      }()),
      altitude_([seed] {
          JavaRandom random{seed + 2U};
          return OctavePerlinNoiseSampler{random, kClimateOctaves};
      }()),
      weirdness_([seed] {
          JavaRandom random{seed + 3U};
          return OctavePerlinNoiseSampler{random, kClimateOctaves};
      }()),
      points_(kNetherPoints) {}

Biome MultiNoiseBiomeSource::sample(int quartX, int quartZ) const {
    // Sample the four climate fields at this quart column. The 0.25 stride keeps a
    // biome region a good few chunks across (a smaller stride shreds it into
    // noise); y is fixed since the nether biome map is 2D per column.
    const double x = static_cast<double>(quartX) * 0.25;
    const double z = static_cast<double>(quartZ) * 0.25;
    const auto temperature = static_cast<float>(temperature_.sample(x, 0.0, z));
    const auto humidity = static_cast<float>(humidity_.sample(x, 0.0, z));
    const auto altitude = static_cast<float>(altitude_.sample(x, 0.0, z));
    const auto weirdness = static_cast<float>(weirdness_.sample(x, 0.0, z));

    Biome best = points_[0].biome;
    float bestDistance = std::numeric_limits<float>::max();
    for (const NoisePoint& point : points_) {
        const float distance = distanceTo(point, temperature, humidity, altitude, weirdness);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = point.biome;
        }
    }
    return best;
}

} // namespace mc::world::gen
