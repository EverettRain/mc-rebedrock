#pragma once

#include <cmath>
#include <cstdint>

// mc::rng — the single deterministic random source for authoritative gameplay.
//
// This is a straight transcription of Java's LegacyRandomSource (== the classic
// java.util.Random): a 48-bit linear congruential generator whose *high* bits
// are the ones handed out. It replaces the scattered 32-bit Numerical-Recipes
// LCG copies that used to live inline in each gameplay system.
//
// Why the Java core, not the old 32-bit LCG:
//   * A power-of-two-modulus LCG's k-th low bit has period <= 2^k, so bit 0
//     flips with period <= 2 — a plain `state % N` for even N leans on the
//     weakest bits and produces a fixed bias (e.g. MobBrain's `% 10` retargeting
//     never firing on even states). Taking the *high* bits via next(bits) side-
//     steps this entirely, and nextInt() rejection-samples away the residual
//     modulo bias for non-power-of-two bounds.
//   * Sharing the vanilla core is a prerequisite for JE sequence parity (tracked
//     separately in JC; per-call-site seeding is out of scope here).
//
// State model: the stored `std::uint64_t` is the raw 48-bit LCG state that a
// call advances in place — it is NOT re-scrambled every call. To *initialise*
// that state from a user seed the way `new java.util.Random(seed)` does, use
// seedFromValue(), which applies Java's setSeed scramble. Decorative RNG
// (particles, audio, renderer) is deliberately left on its own generator and is
// out of this module's scope.
namespace mc::rng {

inline constexpr std::uint64_t kMultiplier = 0x5DEECE66DULL;  // 25214903917
inline constexpr std::uint64_t kIncrement = 0xBULL;           // 11
inline constexpr std::uint64_t kModulusMask = (1ULL << 48) - 1ULL;  // 281474976710655
inline constexpr float kFloatMultiplier = 5.9604645E-8F;      // 1 / 2^24
inline constexpr double kDoubleMultiplier = 1.110223E-16;     // Java's (double)1.110223E-16F

// Java Random(seed): the caller's seed is scrambled before it becomes the LCG
// state. Use this to initialise a stream from a semantic seed so it matches
// java.util.Random(seed) bit-for-bit.
[[nodiscard]] inline std::uint64_t seedFromValue(std::uint64_t seed) {
    return (seed ^ kMultiplier) & kModulusMask;
}

// LegacyRandomSource#next(bits): advance the 48-bit state, then return the top
// `bits` bits as a signed 32-bit int (exactly the Java cast semantics).
[[nodiscard]] inline std::int32_t nextBits(std::uint64_t& state, int bits) {
    state = (state * kMultiplier + kIncrement) & kModulusMask;
    return static_cast<std::int32_t>(state >> (48 - bits));
}

// BitRandomSource#nextInt(): a full 32-bit signed draw.
[[nodiscard]] inline std::int32_t nextInt(std::uint64_t& state) { return nextBits(state, 32); }

// BitRandomSource#nextInt(bound): uniform in [0, bound). A power-of-two bound is
// served by a high-bit multiply; any other bound rejection-samples so the modulo
// carries no bias. `bound` must be positive.
[[nodiscard]] inline std::uint32_t nextInt(std::uint64_t& state, std::uint32_t bound) {
    // (bound & (bound - 1)) == 0 is the power-of-two test Java uses.
    if ((bound & (bound - 1U)) == 0U) {
        const std::int64_t bits = nextBits(state, 31);
        return static_cast<std::uint32_t>((static_cast<std::int64_t>(bound) * bits) >> 31);
    }
    std::int32_t sample = 0;
    std::int32_t modulo = 0;
    do {
        sample = nextBits(state, 31);
        modulo = sample % static_cast<std::int32_t>(bound);
    } while (sample - modulo + (static_cast<std::int32_t>(bound) - 1) < 0);
    return static_cast<std::uint32_t>(modulo);
}

// BitRandomSource#nextFloat(): uniform float in [0, 1) from the top 24 bits.
[[nodiscard]] inline float nextFloat(std::uint64_t& state) {
    return static_cast<float>(nextBits(state, 24)) * kFloatMultiplier;
}

// BitRandomSource#nextDouble(): uniform double in [0, 1).
[[nodiscard]] inline double nextDouble(std::uint64_t& state) {
    const std::int64_t upper = nextBits(state, 26);
    const std::int64_t lower = nextBits(state, 27);
    return static_cast<double>((upper << 27) + lower) * kDoubleMultiplier;
}

// BitRandomSource#nextBoolean().
[[nodiscard]] inline bool nextBoolean(std::uint64_t& state) { return nextBits(state, 1) != 0; }

// MarsagliaPolarGaussian#nextGaussian(): the Java standard-normal draw. It
// caches a second value between calls, so a caller that wants Java-identical
// sequences threads this pair through. `haveNext`/`nextNext` start at
// {false, 0.0} for a fresh stream (matching MarsagliaPolarGaussian#reset).
[[nodiscard]] inline double nextGaussian(std::uint64_t& state, bool& haveNext, double& nextNext) {
    if (haveNext) {
        haveNext = false;
        return nextNext;
    }
    double x = 0.0;
    double y = 0.0;
    double radiusSquared = 0.0;
    do {
        x = 2.0 * nextDouble(state) - 1.0;
        y = 2.0 * nextDouble(state) - 1.0;
        radiusSquared = x * x + y * y;
    } while (radiusSquared >= 1.0 || radiusSquared == 0.0);
    const double multiplier = std::sqrt(-2.0 * std::log(radiusSquared) / radiusSquared);
    nextNext = y * multiplier;
    haveNext = true;
    return x * multiplier;
}

}  // namespace mc::rng
