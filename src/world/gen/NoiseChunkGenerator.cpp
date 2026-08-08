#include "world/gen/NoiseChunkGenerator.hpp"

#include "world/WorldConstants.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace mc::world::gen {
namespace {

// GenerationShapeConfig.OVERWORLD's NoiseSamplingConfig, times the fixed
// 684.412 the generator multiplies every sampling config by.
constexpr double kBaseScale = 684.412;
constexpr double kXzScale = kBaseScale * 0.9999999814507745;
constexpr double kYScale = kBaseScale * 0.9999999814507745;
constexpr double kXzFactor = kXzScale / 80.0;
constexpr double kYFactor = kYScale / 160.0;

// GenerationShapeConfig#getTopSlide / #getBottomSlide.
constexpr double kTopSlideTarget = -10.0;
constexpr int kTopSlideSize = 3;
constexpr int kTopSlideOffset = 0;
constexpr double kBottomSlideTarget = -30.0;
constexpr int kBottomSlideSize = 0;
constexpr int kBottomSlideOffset = 0;

constexpr double kDensityFactor = 1.0;
constexpr double kDensityOffset = -0.46875;

// NoiseChunkGenerator.BIOME_WEIGHT_TABLE, 10 / sqrt(x^2 + z^2 + 0.2) over the
// 5x5 window. Nearby columns dominate, so a biome boundary is a slope rather
// than a cliff.
[[nodiscard]] float biomeWeight(int offsetX, int offsetZ) {
    static const std::array<float, 25> table = [] {
        std::array<float, 25> values{};
        for (int z = -2; z <= 2; ++z) {
            for (int x = -2; x <= 2; ++x) {
                values[static_cast<std::size_t>((x + 2) + (z + 2) * 5)] =
                    10.0F / std::sqrt(static_cast<float>(x * x + z * z) + 0.2F);
            }
        }
        return values;
    }();
    return table[static_cast<std::size_t>((offsetX + 2) + (offsetZ + 2) * 5)];
}

[[nodiscard]] double clampedLerp(double first, double second, double amount) {
    if (amount < 0.0) return first;
    if (amount > 1.0) return second;
    return first + amount * (second - first);
}

[[nodiscard]] double lerp(double amount, double first, double second) {
    return first + amount * (second - first);
}

[[nodiscard]] std::int64_t floorToLong(double value) {
    const auto truncated = static_cast<std::int64_t>(value);
    return value < static_cast<double>(truncated) ? truncated - 1 : truncated;
}

// OctavePerlinNoiseSampler#maintainPrecision.
[[nodiscard]] double maintainPrecision(double value) {
    return value - static_cast<double>(floorToLong(value / 3.3554432E7 + 0.5)) * 3.3554432E7;
}

} // namespace

NoiseChunkGenerator::NoiseChunkGenerator(
    const BiomeSource& biomeSource,
    std::vector<PerlinNoiseSampler> lower,
    std::vector<PerlinNoiseSampler> upper,
    std::vector<PerlinNoiseSampler> interpolation,
    OctavePerlinNoiseSampler densityOffset)
    : biomeSource_(&biomeSource),
      lowerNoise_(std::move(lower)),
      upperNoise_(std::move(upper)),
      interpolationNoise_(std::move(interpolation)),
      densityOffsetNoise_(std::move(densityOffset)) {}

// NoiseChunkGenerator#getRandomDensity, the `randomDensityOffset` term: a very
// slow field sampled at a 200x stride that lifts or drops whole regions by a
// fraction of a block, so two plains chunks never sit at exactly one height.
double NoiseChunkGenerator::randomDensityOffset(int noiseX, int noiseZ) const {
    const double raw = densityOffsetNoise_.sample(
        static_cast<double>(noiseX) * 200.0, 10.0, static_cast<double>(noiseZ) * 200.0, 1.0, 0.0,
        true);
    const double folded = raw < 0.0 ? -raw * 0.3 : raw;
    const double scaled = folded * 24.575625 - 2.0;
    return scaled < 0.0 ? scaled * 0.009486607142857142
                        : std::min(scaled, 1.0) * 0.006640625;
}

std::array<double, 2> NoiseChunkGenerator::noiseRange(int noiseX, int noiseZ) const {
    float scaleSum = 0.0F;
    float depthSum = 0.0F;
    float weightSum = 0.0F;
    const float centreDepth =
        biomeDefinition(biomeSource_->biomeForNoiseGeneration(noiseX, noiseZ)).depth;
    for (int offsetZ = -2; offsetZ <= 2; ++offsetZ) {
        for (int offsetX = -2; offsetX <= 2; ++offsetX) {
            const auto& definition = biomeDefinition(
                biomeSource_->biomeForNoiseGeneration(noiseX + offsetX, noiseZ + offsetZ));
            // A deeper neighbour only counts half, which keeps a mountain from
            // dragging the plain beside it up with it.
            const float bias = definition.depth > centreDepth ? 0.5F : 1.0F;
            const float weight =
                bias * biomeWeight(offsetX, offsetZ) / (definition.depth + 2.0F);
            scaleSum += definition.scale * weight;
            depthSum += definition.depth * weight;
            weightSum += weight;
        }
    }
    const float scale = scaleSum / weightSum;
    const float depth = depthSum / weightSum;
    // GenerationShapeConfig#computeNoiseRange maps the blended depth and scale
    // onto the (depth*0.5 - 0.125) and (scale*0.9 + 0.1) that sampleNoiseColumn
    // feeds into its shape term.
    return {static_cast<double>(depth) * 0.5 - 0.125,
            static_cast<double>(scale) * 0.9 + 0.1};
}

double NoiseChunkGenerator::sampleDensity(int noiseX, int noiseY, int noiseZ) const {
    double lower = 0.0;
    double upper = 0.0;
    double interpolation = 0.0;
    // Octave zero is the finest detail at amplitude one; each following octave
    // halves the frequency and doubles the amplitude, so the last few carry the
    // terrain-scale shape.
    double frequency = 1.0;
    for (int octave = 0; octave < kOctaveCount; ++octave) {
        const double x = maintainPrecision(static_cast<double>(noiseX) * kXzScale * frequency);
        const double y = maintainPrecision(static_cast<double>(noiseY) * kYScale * frequency);
        const double z = maintainPrecision(static_cast<double>(noiseZ) * kXzScale * frequency);
        const double yLattice = kYScale * frequency;
        lower += lowerNoise_[static_cast<std::size_t>(octave)].sample(
                     x, y, z, yLattice, static_cast<double>(noiseY) * yLattice) /
                 frequency;
        upper += upperNoise_[static_cast<std::size_t>(octave)].sample(
                     x, y, z, yLattice, static_cast<double>(noiseY) * yLattice) /
                 frequency;
        if (octave < kInterpolationOctaveCount) {
            const double stretchY = kYFactor * frequency;
            interpolation +=
                interpolationNoise_[static_cast<std::size_t>(octave)].sample(
                    maintainPrecision(static_cast<double>(noiseX) * kXzFactor * frequency),
                    maintainPrecision(static_cast<double>(noiseY) * kYFactor * frequency),
                    maintainPrecision(static_cast<double>(noiseZ) * kXzFactor * frequency),
                    stretchY, static_cast<double>(noiseY) * stretchY) /
                frequency;
        }
        frequency /= 2.0;
    }
    return clampedLerp(lower / 512.0, upper / 512.0, (interpolation / 10.0 + 1.0) / 2.0);
}

double NoiseChunkGenerator::applyShape(
    double density,
    double depth,
    double scale,
    double randomDensityOffset,
    int noiseY) const {
    // SurfaceChunkGenerator#sampleNoiseColumn's shape term: the column's
    // preferred height falls linearly with Y, biased by the biome depth and the
    // random density offset, and scaled by 96/scale. The 4x boost on the
    // positive side is what keeps terrain solid below the surface and lets only
    // the top few cells flip to air.
    const double preferred =
        1.0 - static_cast<double>(noiseY) * 2.0 / static_cast<double>(kNoiseSizeY) +
        randomDensityOffset;
    const double linear = preferred * kDensityFactor + kDensityOffset;
    const double shape = (linear + depth * 0.265625) * (96.0 / scale);
    double shaped = density + (shape > 0.0 ? shape * 4.0 : shape);

    // The top slide pulls the last few noise rows toward solid air so nothing
    // reaches the build limit; the bottom slide does the same in reverse.
    if (kTopSlideSize > 0) {
        const double distance =
            (static_cast<double>(kNoiseSizeY - noiseY) - static_cast<double>(kTopSlideOffset)) /
            static_cast<double>(kTopSlideSize);
        shaped = clampedLerp(kTopSlideTarget, shaped, distance);
    }
    if (kBottomSlideSize > 0) {
        const double distance =
            (static_cast<double>(noiseY) - static_cast<double>(kBottomSlideOffset)) /
            static_cast<double>(kBottomSlideSize);
        shaped = clampedLerp(kBottomSlideTarget, shaped, distance);
    }
    return shaped;
}

void NoiseChunkGenerator::fillNoiseColumn(
    std::array<double, kNoiseSizeY + 1>& column,
    int noiseX,
    int noiseZ) const {
    const auto range = noiseRange(noiseX, noiseZ);
    const double densityOffset = randomDensityOffset(noiseX, noiseZ);
    for (int noiseY = 0; noiseY <= kNoiseSizeY; ++noiseY) {
        column[static_cast<std::size_t>(noiseY)] = applyShape(
            sampleDensity(noiseX, noiseY, noiseZ), range[0], range[1], densityOffset, noiseY);
    }
}

void NoiseChunkGenerator::buildBaseTerrain(Chunk& chunk, int chunkX, int chunkZ) const {
    // Two columns of the lattice at a time, so each cell interpolates between
    // four already-computed corners the way vanilla's rolling buffer does.
    std::array<std::array<double, kNoiseSizeY + 1>, kNoiseSizeZ + 1> previous{};
    std::array<std::array<double, kNoiseSizeY + 1>, kNoiseSizeZ + 1> current{};
    const int baseNoiseX = chunkX * kNoiseSizeX;
    const int baseNoiseZ = chunkZ * kNoiseSizeZ;
    for (int cellZ = 0; cellZ <= kNoiseSizeZ; ++cellZ) {
        fillNoiseColumn(previous[static_cast<std::size_t>(cellZ)], baseNoiseX,
                        baseNoiseZ + cellZ);
    }

    for (int cellX = 0; cellX < kNoiseSizeX; ++cellX) {
        for (int cellZ = 0; cellZ <= kNoiseSizeZ; ++cellZ) {
            fillNoiseColumn(current[static_cast<std::size_t>(cellZ)], baseNoiseX + cellX + 1,
                            baseNoiseZ + cellZ);
        }
        for (int cellZ = 0; cellZ < kNoiseSizeZ; ++cellZ) {
            for (int cellY = kNoiseSizeY - 1; cellY >= 0; --cellY) {
                const double x0z0y0 = previous[static_cast<std::size_t>(cellZ)]
                                             [static_cast<std::size_t>(cellY)];
                const double x0z1y0 = previous[static_cast<std::size_t>(cellZ + 1)]
                                             [static_cast<std::size_t>(cellY)];
                const double x1z0y0 = current[static_cast<std::size_t>(cellZ)]
                                            [static_cast<std::size_t>(cellY)];
                const double x1z1y0 = current[static_cast<std::size_t>(cellZ + 1)]
                                            [static_cast<std::size_t>(cellY)];
                const double x0z0y1 = previous[static_cast<std::size_t>(cellZ)]
                                             [static_cast<std::size_t>(cellY + 1)];
                const double x0z1y1 = previous[static_cast<std::size_t>(cellZ + 1)]
                                             [static_cast<std::size_t>(cellY + 1)];
                const double x1z0y1 = current[static_cast<std::size_t>(cellZ)]
                                            [static_cast<std::size_t>(cellY + 1)];
                const double x1z1y1 = current[static_cast<std::size_t>(cellZ + 1)]
                                            [static_cast<std::size_t>(cellY + 1)];

                for (int blockY = kVerticalNoiseResolution - 1; blockY >= 0; --blockY) {
                    const double deltaY = static_cast<double>(blockY) /
                                          static_cast<double>(kVerticalNoiseResolution);
                    const double z0x0 = lerp(deltaY, x0z0y0, x0z0y1);
                    const double z1x0 = lerp(deltaY, x0z1y0, x0z1y1);
                    const double z0x1 = lerp(deltaY, x1z0y0, x1z0y1);
                    const double z1x1 = lerp(deltaY, x1z1y0, x1z1y1);
                    const int worldY = cellY * kVerticalNoiseResolution + blockY;
                    if (worldY >= kWorldHeight) {
                        continue;
                    }
                    for (int blockX = 0; blockX < kHorizontalNoiseResolution; ++blockX) {
                        const double deltaX = static_cast<double>(blockX) /
                                              static_cast<double>(kHorizontalNoiseResolution);
                        const double z0 = lerp(deltaX, z0x0, z0x1);
                        const double z1 = lerp(deltaX, z1x0, z1x1);
                        for (int blockZ = 0; blockZ < kHorizontalNoiseResolution; ++blockZ) {
                            const double deltaZ =
                                static_cast<double>(blockZ) /
                                static_cast<double>(kHorizontalNoiseResolution);
                            const double density = lerp(deltaZ, z0, z1);
                            const int localX = cellX * kHorizontalNoiseResolution + blockX;
                            const int localZ = cellZ * kHorizontalNoiseResolution + blockZ;
                            if (density > 0.0) {
                                chunk.setBlock(localX, worldY, localZ, Block::Stone);
                            } else if (worldY <= kSeaLevel) {
                                chunk.setBlock(localX, worldY, localZ, Block::Water);
                                chunk.setFluidLevel(localX, worldY, localZ, 0U);
                            }
                        }
                    }
                }
            }
        }
        previous = current;
    }
}

int NoiseChunkGenerator::surfaceHeight(const Chunk& chunk, int localX, int localZ) const {
    for (int y = kWorldHeight - 1; y >= 0; --y) {
        const auto block = chunk.block(localX, y, localZ);
        if (block != Block::Air && block != Block::Water) {
            return y;
        }
    }
    return 0;
}

} // namespace mc::world::gen
