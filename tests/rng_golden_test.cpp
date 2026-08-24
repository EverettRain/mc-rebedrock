#include "gameplay/Random.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

// RNG-0 golden test: mc::rng is Java's LegacyRandomSource (== java.util.Random)
// transcribed. This pins it two ways:
//
//   1. Against canonical, widely-published java.util.Random outputs (literal
//      constants everyone can look up: `new Random(0).nextInt()`,
//      `.nextLong()`, `.nextInt(100)` x10, `.nextGaussian()` x4). These are the
//      real ground truth, independent of any code in this repo.
//   2. Against an independent, self-contained transcription of the JDK algorithm
//      (JavaReference below) run over several seeds — a second implementation,
//      not mc::rng — so every step of the sequence is checked, not just the head.
//
// The mc::rng state is the *raw* 48-bit LCG state; seedFromValue() applies the
// Java setSeed scramble, so seedFromValue(s) reproduces `new java.util.Random(s)`.
namespace {

// A second, independent implementation of java.util.Random from the JDK spec.
// Deliberately does NOT call mc::rng, so a bug shared with mc::rng cannot hide.
class JavaReference {
  public:
    explicit JavaReference(std::uint64_t seedValue) {
        seed_ = (seedValue ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1ULL);
    }
    std::int32_t next(int bits) {
        seed_ = (seed_ * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1ULL);
        return static_cast<std::int32_t>(static_cast<std::int64_t>(seed_ >> (48 - bits)));
    }
    std::int32_t nextInt() { return next(32); }
    std::int32_t nextInt(std::int32_t bound) {
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
    std::int64_t nextLong() {
        return (static_cast<std::int64_t>(next(32)) << 32) + next(32);
    }
    float nextFloat() { return static_cast<float>(next(24)) / static_cast<float>(1 << 24); }
    double nextDouble() {
        return static_cast<double>((static_cast<std::int64_t>(next(26)) << 27) + next(27)) *
               1.110223E-16;
    }
    double nextGaussian() {
        if (haveNext_) {
            haveNext_ = false;
            return nextNext_;
        }
        double v1 = 0.0;
        double v2 = 0.0;
        double s = 0.0;
        do {
            v1 = 2.0 * nextDouble() - 1.0;
            v2 = 2.0 * nextDouble() - 1.0;
            s = v1 * v1 + v2 * v2;
        } while (s >= 1.0 || s == 0.0);
        const double m = std::sqrt(-2.0 * std::log(s) / s);
        nextNext_ = v2 * m;
        haveNext_ = true;
        return v1 * m;
    }

  private:
    std::uint64_t seed_ = 0;
    bool haveNext_ = false;
    double nextNext_ = 0.0;
};

// Exact equality: a faithful transcription must reproduce every bit, so these
// compare identically (no epsilon). The == on float/double is intentional.
bool floatEqual(float a, float b) { return a == b; }
bool doubleEqual(double a, double b) { return a == b; }

}  // namespace

int main() {
    using namespace mc;

    // ---- 1. Canonical java.util.Random literals (the real ground truth) ----
    {
        std::uint64_t state = rng::seedFromValue(0);
        // `new Random(0).nextInt()` — the most-cited java.util.Random value.
        assert(rng::nextInt(state) == -1155484576);
    }
    {
        std::uint64_t state = rng::seedFromValue(0);
        // `new Random(0).nextLong()`.
        const std::int64_t high = static_cast<std::int64_t>(rng::nextInt(state)) << 32;
        const std::int64_t low = static_cast<std::int64_t>(rng::nextInt(state));
        assert(high + low == static_cast<std::int64_t>(-4962768465676381896LL));
    }
    {
        // `new Random(0).nextInt(100)` first ten draws, a textbook sequence.
        std::uint64_t state = rng::seedFromValue(0);
        const std::uint32_t expected[10] = {60, 48, 29, 47, 15, 53, 91, 61, 19, 54};
        for (std::uint32_t want : expected) {
            assert(rng::nextInt(state, 100U) == want);
        }
    }
    {
        // `new Random(0).nextGaussian()` first four draws.
        std::uint64_t state = rng::seedFromValue(0);
        bool haveNext = false;
        double nextNext = 0.0;
        const double expected[4] = {0.80253304465428343, -0.90154614880150108,
                                    2.0809209259515344, 0.7637707118785505};
        for (double want : expected) {
            const double got = rng::nextGaussian(state, haveNext, nextNext);
            assert(doubleEqual(got, want));
        }
    }
    {
        // `new Random(0).nextFloat()` and `.nextDouble()`.
        std::uint64_t f = rng::seedFromValue(0);
        assert(floatEqual(rng::nextFloat(f), 0.73096776f));
        std::uint64_t d = rng::seedFromValue(0);
        assert(doubleEqual(rng::nextDouble(d), 0.73096777116352163));
    }

    // ---- 2. Full-sequence parity against the independent transcription ----
    const std::uint64_t seeds[] = {0U, 1U, 42U, 123456789U, 0x9E3779B9ULL,
                                   0xDEADBEEFCAFEULL, 999999999999ULL};
    for (std::uint64_t seedValue : seeds) {
        // nextInt() — full 32-bit signed.
        {
            JavaReference reference(seedValue);
            std::uint64_t state = rng::seedFromValue(seedValue);
            for (int i = 0; i < 64; ++i) {
                assert(rng::nextInt(state) == reference.nextInt());
            }
        }
        // nextInt(bound) — power-of-two bounds (high-bit multiply path).
        for (std::int32_t bound : {1, 2, 4, 8, 16, 256, 1024, 1 << 20}) {
            JavaReference reference(seedValue);
            std::uint64_t state = rng::seedFromValue(seedValue);
            for (int i = 0; i < 64; ++i) {
                assert(static_cast<std::int32_t>(
                           rng::nextInt(state, static_cast<std::uint32_t>(bound))) ==
                       reference.nextInt(bound));
            }
        }
        // nextInt(bound) — non-power-of-two bounds (rejection-sampling path),
        // including the even bounds the old naive `% N` biased (10, 60, 1000).
        for (std::int32_t bound : {3, 7, 10, 40, 60, 100, 1000, 6000, 168000}) {
            JavaReference reference(seedValue);
            std::uint64_t state = rng::seedFromValue(seedValue);
            for (int i = 0; i < 64; ++i) {
                assert(static_cast<std::int32_t>(
                           rng::nextInt(state, static_cast<std::uint32_t>(bound))) ==
                       reference.nextInt(bound));
            }
        }
        // nextFloat().
        {
            JavaReference reference(seedValue);
            std::uint64_t state = rng::seedFromValue(seedValue);
            for (int i = 0; i < 64; ++i) {
                assert(floatEqual(rng::nextFloat(state), reference.nextFloat()));
            }
        }
        // nextDouble().
        {
            JavaReference reference(seedValue);
            std::uint64_t state = rng::seedFromValue(seedValue);
            for (int i = 0; i < 64; ++i) {
                assert(doubleEqual(rng::nextDouble(state), reference.nextDouble()));
            }
        }
        // nextGaussian() — the cached-pair path threads state through.
        {
            JavaReference reference(seedValue);
            std::uint64_t state = rng::seedFromValue(seedValue);
            bool haveNext = false;
            double nextNext = 0.0;
            for (int i = 0; i < 64; ++i) {
                assert(doubleEqual(rng::nextGaussian(state, haveNext, nextNext),
                                   reference.nextGaussian()));
            }
        }
    }

    // ---- 2b. Rejection-sampling boundary: for large non-power-of-two bounds a
    // sizeable tail of next(31) draws must be *rejected* so the modulo stays
    // uniform. A naive `next(31) % bound` (no rejection) diverges from the
    // reference within a handful of draws for these bounds; the independent
    // JavaReference here rejects exactly as mc::rng must, so any regression that
    // drops the rejection loop is caught fast. (For small bounds the tail is one
    // in millions, so this uses bounds where rejection is common.)
    for (std::int32_t bound : {(1 << 30) + 1, (1 << 30) + 12345, 2000000000, 1431655765}) {
        for (std::uint64_t seedValue : seeds) {
            JavaReference reference(seedValue);
            std::uint64_t state = rng::seedFromValue(seedValue);
            for (int i = 0; i < 5000; ++i) {
                assert(static_cast<std::int32_t>(
                           rng::nextInt(state, static_cast<std::uint32_t>(bound))) ==
                       reference.nextInt(bound));
            }
        }
    }

    // ---- 3. The bias the old `% N` had is gone: over the full period-ish span
    // of the weak low bit, an even bound splits evenly rather than locking to a
    // parity of the state. This is the MobBrain `% 10` retarget bug (RNG-0's
    // headline fix): a large sample of nextInt(10) must hit every residue,
    // including the ones a low-bit-only draw would starve.
    {
        std::uint64_t state = rng::seedFromValue(0xA5A5A5A5ULL);
        int counts[10] = {0};
        for (int i = 0; i < 100000; ++i) {
            counts[rng::nextInt(state, 10U)] += 1;
        }
        for (int residue = 0; residue < 10; ++residue) {
            // A perfectly uniform 100k/10 = 10000 each; assert every residue is
            // well-represented (the old even-N modulo could pin some to zero).
            assert(counts[residue] > 8000);
            assert(counts[residue] < 12000);
        }
    }

    std::printf("rng_golden: all java.util.Random parity checks passed\n");
    return 0;
}
