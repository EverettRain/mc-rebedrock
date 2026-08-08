#pragma once

#include <cmath>
#include <cstdint>

namespace mc::world::gen {

// java.util.Random, bit for bit. Every generator below is seeded and stepped
// exactly the way Minecraft does it, so a given seed lays its noise, its caves
// and its ores down in the same order Java would; reproducing that order is the
// whole point of porting the algorithm rather than inventing one.
class JavaRandom final {
  public:
    explicit JavaRandom(std::uint64_t seed = 0U) { setSeed(seed); }

    void setSeed(std::uint64_t seed) {
        state_ = (seed ^ 0x5DEECE66DULL) & kMask;
        haveNextGaussian_ = false;
    }

    [[nodiscard]] std::int32_t next(int bits) {
        state_ = (state_ * 0x5DEECE66DULL + 0xBULL) & kMask;
        return static_cast<std::int32_t>(state_ >> (48 - bits));
    }

    [[nodiscard]] std::int32_t nextInt() { return next(32); }

    [[nodiscard]] std::int32_t nextInt(std::int32_t bound) {
        if (bound <= 0) {
            return 0;
        }
        // Powers of two take the high bits directly; everything else rejects the
        // tail that would bias the modulo.
        if ((bound & -bound) == bound) {
            return static_cast<std::int32_t>(
                (static_cast<std::int64_t>(bound) * next(31)) >> 31);
        }
        std::int32_t bits = 0;
        std::int32_t value = 0;
        do {
            bits = next(31);
            value = bits % bound;
        } while (bits - value + (bound - 1) < 0);
        return value;
    }

    // Java's nextInt(origin, bound) idiom used all over the world generator.
    [[nodiscard]] std::int32_t nextBetween(std::int32_t minimum, std::int32_t maximum) {
        return minimum + nextInt(maximum - minimum + 1);
    }

    [[nodiscard]] std::int64_t nextLong() {
        const std::int64_t high = static_cast<std::int64_t>(next(32)) << 32;
        return high + next(32);
    }

    [[nodiscard]] bool nextBoolean() { return next(1) != 0; }

    [[nodiscard]] float nextFloat() {
        return static_cast<float>(next(24)) / static_cast<float>(1 << 24);
    }

    [[nodiscard]] double nextDouble() {
        const std::int64_t high = static_cast<std::int64_t>(next(26)) << 27;
        return static_cast<double>(high + next(27)) * (1.0 / 9007199254740992.0);
    }

    [[nodiscard]] double nextGaussian() {
        if (haveNextGaussian_) {
            haveNextGaussian_ = false;
            return nextGaussian_;
        }
        // The polar method, exactly as java.util.Random writes it.
        double first = 0.0;
        double second = 0.0;
        double lengthSquared = 0.0;
        do {
            first = 2.0 * nextDouble() - 1.0;
            second = 2.0 * nextDouble() - 1.0;
            lengthSquared = first * first + second * second;
        } while (lengthSquared >= 1.0 || lengthSquared == 0.0);
        const double multiplier = std::sqrt(-2.0 * std::log(lengthSquared) / lengthSquared);
        nextGaussian_ = second * multiplier;
        haveNextGaussian_ = true;
        return first * multiplier;
    }

    // ChunkRandom#consume: burns draws so a generator that skips an octave still
    // leaves the stream where vanilla leaves it.
    void consume(int count) {
        for (int index = 0; index < count; ++index) {
            state_ = (state_ * 0x5DEECE66DULL + 0xBULL) & kMask;
        }
    }

    // ChunkRandom#setTerrainSeed: the per-chunk seed the decoration passes use.
    std::int64_t setTerrainSeed(int chunkX, int chunkZ) {
        const std::int64_t seed = static_cast<std::int64_t>(chunkX) * 341873128712LL +
                                  static_cast<std::int64_t>(chunkZ) * 132897987541LL;
        setSeed(static_cast<std::uint64_t>(seed));
        return seed;
    }

    // ChunkRandom#setCarverSeed: mixes the world seed with the chunk being
    // carved, so a carver started in one chunk reaches into its neighbours the
    // same way whichever chunk is generated first.
    void setCarverSeed(std::uint64_t worldSeed, int chunkX, int chunkZ) {
        setSeed(worldSeed);
        const std::int64_t xMultiplier = nextLong();
        const std::int64_t zMultiplier = nextLong();
        const std::int64_t seed = (static_cast<std::int64_t>(chunkX) * xMultiplier) ^
                                  (static_cast<std::int64_t>(chunkZ) * zMultiplier) ^
                                  static_cast<std::int64_t>(worldSeed);
        setSeed(static_cast<std::uint64_t>(seed));
    }

    // ChunkRandom#setDecoratorSeed: each feature step gets its own stream.
    void setDecoratorSeed(std::int64_t populationSeed, int index, int step) {
        setSeed(static_cast<std::uint64_t>(
            populationSeed + static_cast<std::int64_t>(index) +
            static_cast<std::int64_t>(10000 * step)));
    }

    // ChunkRandom#setPopulationSeed: the base every decorator seed is derived
    // from, drawn once per chunk.
    std::int64_t setPopulationSeed(std::uint64_t worldSeed, int blockX, int blockZ) {
        setSeed(worldSeed);
        const std::int64_t xMultiplier = nextLong() | 1LL;
        const std::int64_t zMultiplier = nextLong() | 1LL;
        const std::int64_t seed = (static_cast<std::int64_t>(blockX) * xMultiplier +
                                   static_cast<std::int64_t>(blockZ) * zMultiplier) ^
                                  static_cast<std::int64_t>(worldSeed);
        setSeed(static_cast<std::uint64_t>(seed));
        return seed;
    }

  private:
    static constexpr std::uint64_t kMask = (1ULL << 48) - 1ULL;

    std::uint64_t state_ = 0U;
    double nextGaussian_ = 0.0;
    bool haveNextGaussian_ = false;
};

} // namespace mc::world::gen
