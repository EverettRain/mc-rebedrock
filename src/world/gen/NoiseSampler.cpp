#include "world/gen/NoiseSampler.hpp"

#include <algorithm>
#include <cmath>

namespace mc::world::gen {
namespace {

// SimplexNoiseSampler.GRADIENTS, shared by both samplers.
constexpr std::array<std::array<int, 3>, 16> kGradients{{
    {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0},
    {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
    {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1},
    {1, 1, 0}, {0, -1, 1}, {-1, 1, 0}, {0, -1, -1},
}};

[[nodiscard]] double dotGradient(const std::array<int, 3>& gradient, double x, double y, double z) {
    return static_cast<double>(gradient[0]) * x + static_cast<double>(gradient[1]) * y +
           static_cast<double>(gradient[2]) * z;
}

[[nodiscard]] double gradient(int hash, double x, double y, double z) {
    return dotGradient(kGradients[static_cast<std::size_t>(hash & 15)], x, y, z);
}

// MathHelper#perlinFade.
[[nodiscard]] double fade(double value) {
    return value * value * value * (value * (value * 6.0 - 15.0) + 10.0);
}

[[nodiscard]] double lerp(double amount, double first, double second) {
    return first + amount * (second - first);
}

[[nodiscard]] std::int64_t floorToLong(double value) {
    const auto truncated = static_cast<std::int64_t>(value);
    return value < static_cast<double>(truncated) ? truncated - 1 : truncated;
}

[[nodiscard]] int floorToInt(double value) {
    return static_cast<int>(floorToLong(value));
}

// OctavePerlinNoiseSampler#maintainPrecision: folds huge coordinates back
// toward the origin so a double still resolves individual blocks far out.
[[nodiscard]] double maintainPrecision(double value) {
    return value - static_cast<double>(floorToLong(value / 3.3554432E7 + 0.5)) * 3.3554432E7;
}

// The Fisher-Yates shuffle both samplers run over their permutation table.
void shufflePermutation(JavaRandom& random, std::array<int, 256>& permutation) {
    for (int index = 0; index < 256; ++index) {
        permutation[static_cast<std::size_t>(index)] = index;
    }
    for (int index = 0; index < 256; ++index) {
        const int offset = random.nextInt(256 - index);
        std::swap(permutation[static_cast<std::size_t>(index)],
                  permutation[static_cast<std::size_t>(index + offset)]);
    }
}

} // namespace

PerlinNoiseSampler::PerlinNoiseSampler(JavaRandom& random) {
    originX_ = random.nextDouble() * 256.0;
    originY_ = random.nextDouble() * 256.0;
    originZ_ = random.nextDouble() * 256.0;
    shufflePermutation(random, permutation_);
}

double PerlinNoiseSampler::sample(
    double x,
    double y,
    double z,
    double yScale,
    double yMax) const {
    const double shiftedX = x + originX_;
    const double shiftedY = y + originY_;
    const double shiftedZ = z + originZ_;
    const int cellX = floorToInt(shiftedX);
    const int cellY = floorToInt(shiftedY);
    const int cellZ = floorToInt(shiftedZ);
    const double localX = shiftedX - static_cast<double>(cellX);
    double localY = shiftedY - static_cast<double>(cellY);
    const double localZ = shiftedZ - static_cast<double>(cellZ);
    const double fadeY = localY;
    if (yScale != 0.0) {
        // The density noise quantises Y onto a coarser lattice than X and Z,
        // which is what gives 1.16 terrain its horizontal banding.
        const double clamped = (yMax >= 0.0 && yMax < localY) ? yMax : localY;
        localY -= std::floor(clamped / yScale + 1.0E-7) * yScale;
    }

    const int hashX = permute(cellX);
    const int hashX1 = permute(cellX + 1);
    const int hashXY = permute(hashX + cellY);
    const int hashXY1 = permute(hashX + cellY + 1);
    const int hashX1Y = permute(hashX1 + cellY);
    const int hashX1Y1 = permute(hashX1 + cellY + 1);

    const double c000 = gradient(permute(hashXY + cellZ), localX, localY, localZ);
    const double c100 = gradient(permute(hashX1Y + cellZ), localX - 1.0, localY, localZ);
    const double c010 = gradient(permute(hashXY1 + cellZ), localX, localY - 1.0, localZ);
    const double c110 = gradient(permute(hashX1Y1 + cellZ), localX - 1.0, localY - 1.0, localZ);
    const double c001 = gradient(permute(hashXY + cellZ + 1), localX, localY, localZ - 1.0);
    const double c101 = gradient(permute(hashX1Y + cellZ + 1), localX - 1.0, localY, localZ - 1.0);
    const double c011 = gradient(permute(hashXY1 + cellZ + 1), localX, localY - 1.0, localZ - 1.0);
    const double c111 =
        gradient(permute(hashX1Y1 + cellZ + 1), localX - 1.0, localY - 1.0, localZ - 1.0);

    const double fadedX = fade(localX);
    const double fadedY = fade(fadeY);
    const double fadedZ = fade(localZ);
    return lerp(fadedZ,
                lerp(fadedY, lerp(fadedX, c000, c100), lerp(fadedX, c010, c110)),
                lerp(fadedY, lerp(fadedX, c001, c101), lerp(fadedX, c011, c111)));
}

OctavePerlinNoiseSampler::OctavePerlinNoiseSampler(JavaRandom& random, int octaveCount) {
    octaves_ = buildOctaves(random, std::max(octaveCount, 1));
}

double OctavePerlinNoiseSampler::sample(
    double x,
    double y,
    double z,
    double yScale,
    double yMax,
    bool useOrigin) const {
    // OctavePerlinNoiseSampler#sample: the first-drawn octave reads at
    // frequency 1 with the smallest weight, and each following octave halves
    // its frequency while its weight doubles, so the stack stays normalised to
    // [-1, 1].
    double total = 0.0;
    double frequency = 1.0;
    double amplitude = 1.0 / (std::pow(2.0, static_cast<double>(octaves_.size())) - 1.0);
    for (const auto& octave : octaves_) {
        total += octave.sample(
                     maintainPrecision(x * frequency),
                     useOrigin ? -octave.originY() : maintainPrecision(y * frequency),
                     maintainPrecision(z * frequency), yScale * frequency, yMax * frequency) *
                 amplitude;
        frequency /= 2.0;
        amplitude *= 2.0;
    }
    return total;
}

SimplexNoiseSampler::SimplexNoiseSampler(JavaRandom& random) {
    originX_ = random.nextDouble() * 256.0;
    originY_ = random.nextDouble() * 256.0;
    originZ_ = random.nextDouble() * 256.0;
    shufflePermutation(random, permutation_);
}

double SimplexNoiseSampler::sample(double x, double y) const {
    // SimplexNoiseSampler#sample(double, double): the classic 2D simplex skew.
    constexpr double kSkew = 0.5 * (1.7320508075688772 - 1.0);
    constexpr double kUnskew = (3.0 - 1.7320508075688772) / 6.0;
    const double shiftedX = x + originX_;
    const double shiftedY = y + originY_;
    const double skew = (shiftedX + shiftedY) * kSkew;
    const int cellX = floorToInt(shiftedX + skew);
    const int cellY = floorToInt(shiftedY + skew);
    const double unskew = static_cast<double>(cellX + cellY) * kUnskew;
    const double localX = shiftedX - (static_cast<double>(cellX) - unskew);
    const double localY = shiftedY - (static_cast<double>(cellY) - unskew);
    const int cornerX = localX > localY ? 1 : 0;
    const int cornerY = localX > localY ? 0 : 1;
    const double midX = localX - static_cast<double>(cornerX) + kUnskew;
    const double midY = localY - static_cast<double>(cornerY) + kUnskew;
    const double farX = localX - 1.0 + 2.0 * kUnskew;
    const double farY = localY - 1.0 + 2.0 * kUnskew;
    const int wrappedX = cellX & 255;
    const int wrappedY = cellY & 255;
    const int hash0 = permute(wrappedX + permute(wrappedY)) & 15;
    const int hash1 = permute(wrappedX + cornerX + permute(wrappedY + cornerY)) & 15;
    const int hash2 = permute(wrappedX + 1 + permute(wrappedY + 1)) & 15;

    const auto corner = [](int hash, double dx, double dy) {
        double falloff = 0.5 - dx * dx - dy * dy;
        if (falloff < 0.0) {
            return 0.0;
        }
        falloff *= falloff;
        return falloff * falloff * dotGradient(kGradients[static_cast<std::size_t>(hash)], dx, dy, 0.0);
    };

    return 70.0 * (corner(hash0, localX, localY) + corner(hash1, midX, midY) +
                   corner(hash2, farX, farY));
}

OctaveSimplexNoiseSampler::OctaveSimplexNoiseSampler(JavaRandom& random, int octaveCount) {
    octaveCount = std::max(octaveCount, 1);
    octaves_.reserve(static_cast<std::size_t>(octaveCount));
    for (int octave = 0; octave < octaveCount; ++octave) {
        octaves_.emplace_back(random);
    }
}

double OctaveSimplexNoiseSampler::sample(double x, double y) const {
    // OctaveSimplexNoiseSampler#sample(x, y, true): the lowest octave carries
    // the largest weight, and each following octave halves its frequency while
    // its weight doubles, so the stack stays normalised to [-1, 1].
    double total = 0.0;
    double frequency = 1.0;
    double amplitude = 1.0 / (std::pow(2.0, static_cast<double>(octaves_.size())) - 1.0);
    for (const auto& octave : octaves_) {
        total += octave.sample(x * frequency, y * frequency) * amplitude;
        frequency /= 2.0;
        amplitude *= 2.0;
    }
    return total;
}

} // namespace mc::world::gen
