#include "world/gen/LayeredBiomeSource.hpp"

#include "world/gen/JavaRandom.hpp"
#include "world/gen/NoiseSampler.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

namespace mc::world::gen {
namespace {

// ---------------------------------------------------------------------------
// Layer-internal biome ids. These are the ids the GenLayer pipeline carries;
// the top sampler maps them onto mc::world::gen::Biome (hills and temperature
// variants collapse onto their base biome). 1..4 are the climate tokens the
// early layers work with: Plains, Desert, Mountains, Forest, exactly as in the
// vanilla pipeline (biomes are raw registry ids there too).
constexpr int kOcean = 0;
constexpr int kPlains = 1;
constexpr int kDesert = 2;
constexpr int kMountains = 3;
constexpr int kForest = 4;
constexpr int kTaiga = 5;
constexpr int kSwamp = 6;
constexpr int kRiver = 7;
constexpr int kSnowyTundra = 8;
constexpr int kSnowyTaiga = 9;
constexpr int kBirchForest = 10;
constexpr int kDarkForest = 11;
constexpr int kSavanna = 12;
constexpr int kJungle = 13;
constexpr int kBeach = 14;
constexpr int kSnowyBeach = 15;
constexpr int kStoneShore = 16;
constexpr int kMushroomFields = 17;
constexpr int kMushroomShore = 18;
constexpr int kDeepOcean = 19;
constexpr int kWarmOcean = 20;
constexpr int kLukewarmOcean = 21;
constexpr int kColdOcean = 22;
constexpr int kFrozenOcean = 23;
constexpr int kDeepWarmOcean = 24;
constexpr int kDeepLukewarmOcean = 25;
constexpr int kDeepColdOcean = 26;
constexpr int kDeepFrozenOcean = 27;
constexpr int kBadlands = 28;
constexpr int kWoodedBadlandsPlateau = 29;
constexpr int kGiantTreeTaiga = 30;
constexpr int kBadlandsPlateau = 31;
constexpr int kJungleEdge = 32;
constexpr int kSavannaPlateau = 33;
constexpr int kSunflowerPlains = 34;
constexpr int kFrozenRiver = 35;
constexpr int kBambooJungle = 36;

// BiomeLayers#isOcean / #isShallowOcean.
[[nodiscard]] bool isShallowOcean(int id) {
    return id == kOcean || id == kWarmOcean || id == kLukewarmOcean || id == kColdOcean ||
           id == kFrozenOcean;
}

[[nodiscard]] bool isOcean(int id) {
    return isShallowOcean(id) || id == kDeepOcean || id == kDeepWarmOcean ||
           id == kDeepLukewarmOcean || id == kDeepColdOcean || id == kDeepFrozenOcean;
}

[[nodiscard]] bool isBadlandsLike(int id) {
    return id == kBadlands || id == kWoodedBadlandsPlateau || id == kBadlandsPlateau;
}

// The biome category, used by BiomeLayers#areSimilar.
enum class Category : std::uint8_t {
    None,
    Ocean,
    Plains,
    Desert,
    Mountains,
    Forest,
    Taiga,
    Swamp,
    River,
    SnowyTundra,
    Savanna,
    Jungle,
    Beach,
    Mushroom,
};

[[nodiscard]] Category category(int id) {
    if (isOcean(id)) return Category::Ocean;
    switch (id) {
    case kPlains: case kSunflowerPlains: return Category::Plains;
    case kDesert: case kBadlands: case kWoodedBadlandsPlateau: case kBadlandsPlateau:
        return Category::Desert;
    case kMountains: case kStoneShore: return Category::Mountains;
    case kForest: case kBirchForest: case kDarkForest: return Category::Forest;
    case kTaiga: case kSnowyTaiga: case kGiantTreeTaiga: return Category::Taiga;
    case kSwamp: return Category::Swamp;
    case kRiver: case kFrozenRiver: return Category::River;
    case kSnowyTundra: return Category::SnowyTundra;
    case kSavanna: case kSavannaPlateau: return Category::Savanna;
    case kJungle: case kJungleEdge: case kBambooJungle: return Category::Jungle;
    case kBeach: case kSnowyBeach: return Category::Beach;
    case kMushroomFields: case kMushroomShore: return Category::Mushroom;
    default: return Category::None;
    }
}

// BiomeLayers#areSimilar: same id, or the two biomes share a non-trivial
// category (the badlands plateaus count as similar to each other on their own).
[[nodiscard]] bool areSimilar(int a, int b) {
    if (a == b) return true;
    if (a == kBadlandsPlateau || a == kWoodedBadlandsPlateau) {
        return b == kBadlandsPlateau || b == kWoodedBadlandsPlateau;
    }
    const Category ca = category(a);
    const Category cb = category(b);
    return ca != Category::None && cb != Category::None && ca == cb;
}

// The temperature band EaseBiomeEdgeLayer#method_15839 compares: frozen, cold,
// medium, warm. Only the distinction between "adjacent to snow" and the rest
// matters to the reduced biome set.
[[nodiscard]] int temperatureBand(int id) {
    switch (id) {
    case kSnowyTundra: case kSnowyTaiga: case kFrozenOcean: case kDeepFrozenOcean:
    case kSnowyBeach: case kFrozenRiver:
        return 0; // frozen
    case kTaiga: case kColdOcean: case kDeepColdOcean:
        return 1; // cold
    case kDesert: case kSavanna: case kSavannaPlateau: case kJungle: case kJungleEdge:
    case kBambooJungle: case kBadlands: case kWoodedBadlandsPlateau: case kBadlandsPlateau:
    case kWarmOcean: case kDeepWarmOcean:
        return 3; // warm
    default:
        return 2; // medium
    }
}

// EaseBiomeEdgeLayer#method_15839: similar, or same temperature band, or either
// is the medium band (which neighbours anything).
[[nodiscard]] bool easeEdgeSimilar(int a, int b) {
    if (areSimilar(a, b)) return true;
    const int ta = temperatureBand(a);
    const int tb = temperatureBand(b);
    return ta == tb || ta == 2 || tb == 2;
}

// AddEdgeBiomesLayer#isWooded.
[[nodiscard]] bool isWooded(int id) {
    return category(id) == Category::Jungle || id == kJungleEdge || id == kJungle ||
           id == kForest || id == kTaiga || isOcean(id);
}

// AddEdgeBiomesLayer: biomes with snow precipitation.
[[nodiscard]] bool isSnowy(int id) {
    return id == kSnowyTundra || id == kSnowyTaiga;
}

// ---------------------------------------------------------------------------
// Seed mixing (SeedMixer#mixSeed) and 64-bit wrapping arithmetic, matching
// Java's long overflow behaviour.
[[nodiscard]] std::int64_t mixSeed(std::int64_t seedIn, std::int64_t salt) {
    auto seed = static_cast<std::uint64_t>(seedIn);
    seed *= seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::int64_t>(seed) + salt;
}

[[nodiscard]] std::int64_t addSalt(std::uint64_t seed, std::int64_t salt) {
    std::int64_t l = salt;
    l = mixSeed(l, salt);
    l = mixSeed(l, salt);
    l = mixSeed(l, salt);
    std::int64_t m = static_cast<std::int64_t>(seed);
    m = mixSeed(m, l);
    m = mixSeed(m, l);
    return mixSeed(m, l);
}

[[nodiscard]] std::int64_t floorModLong(std::int64_t x, std::int64_t y) {
    std::int64_t r = x % y;
    if (r < 0) r += y;
    return r;
}

[[nodiscard]] std::uint64_t toLong(int x, int z) {
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) << 32U);
}

// ---------------------------------------------------------------------------
// One CachingLayerContext per layer: its salted world seed, the per-cell
// local seed, the shared Perlin sampler (OceanTemperatureLayer) and the shared
// cell cache every sampler this context creates draws on.
struct LayerCache final {
    std::unordered_map<std::uint64_t, int> values;
    void trim(int capacity) {
        // Values are pure functions of the cell, so dropping everything and
        // recomputing on demand is correct; it just bounds the memory.
        if (static_cast<int>(values.size()) > capacity) values.clear();
    }
};

struct LayerContext final {
    std::int64_t worldSeed;
    std::int64_t localSeed = 0;
    PerlinNoiseSampler noise;
    std::shared_ptr<LayerCache> cache = std::make_shared<LayerCache>();
    int cacheCapacity;

    LayerContext(std::uint64_t seed, std::int64_t salt)
        : worldSeed(addSalt(seed, salt)),
          noise([](std::uint64_t s) {
              JavaRandom random{s};
              return PerlinNoiseSampler{random};
          }(seed)),
          cacheCapacity(25) {}

    void initSeed(int x, int y) {
        std::int64_t l = worldSeed;
        l = mixSeed(l, x);
        l = mixSeed(l, y);
        l = mixSeed(l, x);
        l = mixSeed(l, y);
        localSeed = l;
    }

    int nextInt(int bound) {
        const int value = static_cast<int>(
            floorModLong(static_cast<std::int64_t>(localSeed >> 24),
                         static_cast<std::int64_t>(bound)));
        localSeed = mixSeed(localSeed, worldSeed);
        return value;
    }

    int choose(int a, int b) { return nextInt(2) == 0 ? a : b; }

    int choose(int a, int b, int c, int d) {
        const int i = nextInt(4);
        if (i == 0) return a;
        if (i == 1) return b;
        return i == 2 ? c : d;
    }
};

// A CachingLayerSampler: caches (x, z) cells, sharing the LayerContext's cache
// with every other sampler the same context created.
//
// Two known departures from vanilla, both deliberately left alone: `sample` is
// const but writes the cache (safe only because each chunk streamer runs one
// worker and owns its generator), and `trim` clears the whole map instead of
// evicting one entry, so a shared 25-cell budget thrashes. Vanilla gives each
// sampler its own 25-entry LRU. This is 1.16's GenLayer stack, which 26.1 no
// longer has at all — BM-7 deletes this file rather than tuning it, so don't
// spend effort here (BM-DESIGN 判断 1).
class CachingSampler final {
  public:
    CachingSampler(std::shared_ptr<LayerCache> cache, int capacity,
                   std::function<int(int, int)> op)
        : cache_(std::move(cache)), capacity_(capacity), op_(std::move(op)) {}

    [[nodiscard]] int sample(int x, int z) const {
        const std::uint64_t key = toLong(x, z);
        if (const auto it = cache_->values.find(key); it != cache_->values.end()) {
            return it->second;
        }
        const int value = op_(x, z);
        cache_->values.emplace(key, value);
        cache_->trim(capacity_);
        return value;
    }

    [[nodiscard]] int capacity() const { return capacity_; }

  private:
    std::shared_ptr<LayerCache> cache_;
    int capacity_;
    std::function<int(int, int)> op_;
};

using Factory = std::function<CachingSampler()>;

[[nodiscard]] std::shared_ptr<LayerContext> makeContext(std::uint64_t seed,
                                                        std::int64_t salt) {
    return std::make_shared<LayerContext>(seed, salt);
}

// InitLayer#create (ContinentLayer, OceanTemperatureLayer).
[[nodiscard]] Factory makeInit(std::shared_ptr<LayerContext> ctx,
                               std::function<int(LayerContext&, int, int)> sample) {
    return [ctx, sample = std::move(sample)] {
        return CachingSampler{ctx->cache, ctx->cacheCapacity,
                              [ctx, sample](int x, int z) {
                                  ctx->initSeed(x, z);
                                  return sample(*ctx, x, z);
                              }};
    };
}

// ParentedLayer#create.
[[nodiscard]] Factory makeParented(
    std::shared_ptr<LayerContext> ctx, Factory parent,
    std::function<int(LayerContext&, const CachingSampler&, int, int)> sample) {
    return [ctx, parent = std::move(parent), sample = std::move(sample)] {
        CachingSampler par = parent();
        const int capacity = std::min(1024, par.capacity() * 4);
        return CachingSampler{ctx->cache, capacity,
                              [ctx, par = std::move(par), sample](int x, int z) {
                                  ctx->initSeed(x, z);
                                  return sample(*ctx, par, x, z);
                              }};
    };
}

// MergingLayer#create (AddHillsLayer, AddRiversLayer, ApplyOceanTemperatureLayer).
[[nodiscard]] Factory makeMerging(
    std::shared_ptr<LayerContext> ctx, Factory first, Factory second,
    std::function<int(LayerContext&, const CachingSampler&, const CachingSampler&, int, int)>
        sample) {
    return [ctx, first = std::move(first), second = std::move(second),
            sample = std::move(sample)] {
        CachingSampler a = first();
        CachingSampler b = second();
        const int capacity = std::min(1024, std::max(a.capacity(), b.capacity()) * 4);
        return CachingSampler{
            ctx->cache, capacity,
            [ctx, a = std::move(a), b = std::move(b), sample](int x, int z) {
                ctx->initSeed(x, z);
                return sample(*ctx, a, b, x, z);
            }};
    };
}

// ---------------------------------------------------------------------------
// Layer operations. Each mirrors the vanilla layer class of the same
// name; the sampling coordinates are the CrossSamplingLayer / DiagonalCross /
// Identity / SouthEast neighbourhoods.

// ContinentLayer.
int continentSample(LayerContext& c, int x, int y) {
    if (x == 0 && y == 0) return kPlains;
    return c.nextInt(10) == 0 ? kPlains : kOcean;
}

// ScaleLayer.
int scaleSample(LayerContext& c, const CachingSampler& parent, int x, int z, bool fuzzy) {
    const int top = parent.sample(x >> 1, z >> 1);
    c.initSeed(x >> 1 << 1, z >> 1 << 1);
    const int j = x & 1;
    const int k = z & 1;
    if (j == 0 && k == 0) return top;
    const int l = parent.sample(x >> 1, (z + 1) >> 1);
    const int m = c.choose(top, l);
    if (j == 0 && k == 1) return m;
    const int n = parent.sample((x + 1) >> 1, z >> 1);
    const int o = c.choose(top, n);
    if (j == 1 && k == 0) return o;
    const int p = parent.sample((x + 1) >> 1, (z + 1) >> 1);
    if (fuzzy) return c.choose(top, n, l, p);
    if (n == l && l == p) return n;
    if (top == n && top == l) return top;
    if (top == n && top == p) return top;
    if (top == l && top == p) return top;
    if (top == n && l != p) return top;
    if (top == l && n != p) return top;
    if (top == p && n != l) return top;
    if (n == l && top != p) return n;
    if (n == p && top != l) return n;
    return l == p && top != n ? l : c.choose(top, n, l, p);
}

// IncreaseEdgeCurvatureLayer.
int increaseEdgeCurvatureSample(LayerContext& c, int sw, int se, int ne, int nw, int center) {
    if (!isShallowOcean(center) ||
        (isShallowOcean(nw) && isShallowOcean(ne) && isShallowOcean(sw) && isShallowOcean(se))) {
        if (!isShallowOcean(center) &&
            (isShallowOcean(nw) || isShallowOcean(sw) || isShallowOcean(ne) ||
             isShallowOcean(se)) &&
            c.nextInt(5) == 0) {
            if (isShallowOcean(nw)) return center == kForest ? kForest : nw;
            if (isShallowOcean(sw)) return center == kForest ? kForest : sw;
            if (isShallowOcean(ne)) return center == kForest ? kForest : ne;
            if (isShallowOcean(se)) return center == kForest ? kForest : se;
        }
        return center;
    }
    int i = 1;
    int j = 1;
    if (!isShallowOcean(nw) && c.nextInt(i++) == 0) j = nw;
    if (!isShallowOcean(ne) && c.nextInt(i++) == 0) j = ne;
    if (!isShallowOcean(sw) && c.nextInt(i++) == 0) j = sw;
    if (!isShallowOcean(se) && c.nextInt(i++) == 0) j = se;
    if (c.nextInt(3) == 0) return j;
    return j == kForest ? kForest : center;
}

// AddIslandLayer.
int addIslandSample(LayerContext& c, int n, int e, int s, int w, int center) {
    return isShallowOcean(center) && isShallowOcean(n) && isShallowOcean(e) &&
                   isShallowOcean(w) && isShallowOcean(s) && c.nextInt(2) == 0
               ? kPlains
               : center;
}

// AddDeepOceanLayer.
int addDeepOceanSample(LayerContext& c, int n, int e, int s, int w, int center) {
    if (isShallowOcean(center)) {
        int count = 0;
        if (isShallowOcean(n)) ++count;
        if (isShallowOcean(e)) ++count;
        if (isShallowOcean(w)) ++count;
        if (isShallowOcean(s)) ++count;
        if (count > 3) {
            if (center == kWarmOcean) return kDeepWarmOcean;
            if (center == kLukewarmOcean) return kDeepLukewarmOcean;
            if (center == kOcean) return kDeepOcean;
            if (center == kColdOcean) return kDeepColdOcean;
            if (center == kFrozenOcean) return kDeepFrozenOcean;
            return kDeepOcean;
        }
    }
    return center;
}

// OceanTemperatureLayer.
int oceanTemperatureSample(LayerContext& c, int x, int y) {
    const double d = c.noise.sample(static_cast<double>(x) / 8.0, static_cast<double>(y) / 8.0,
                                    0.0, 0.0, 0.0);
    if (d > 0.4) return kWarmOcean;
    if (d > 0.2) return kLukewarmOcean;
    if (d < -0.4) return kFrozenOcean;
    return d < -0.2 ? kColdOcean : kOcean;
}

// AddColdClimatesLayer.
int addColdClimatesSample(LayerContext& c, int se) {
    if (isShallowOcean(se)) return se;
    const int i = c.nextInt(6);
    if (i == 0) return kForest;
    return i == 1 ? kMountains : kPlains;
}

// AddClimateLayers.AddTemperateBiomesLayer.
int addTemperateSample(LayerContext& c, int n, int e, int s, int w, int center) {
    const bool nearMountainOrForest =
        n == kMountains || e == kMountains || w == kMountains || s == kMountains ||
        n == kForest || e == kForest || w == kForest || s == kForest;
    return center != kPlains || !nearMountainOrForest ? center : kDesert;
}

// AddClimateLayers.AddCoolBiomesLayer.
int addCoolSample(LayerContext& c, int n, int e, int s, int w, int center) {
    const bool nearPlainsOrDesert =
        n == kPlains || e == kPlains || w == kPlains || s == kPlains || n == kDesert ||
        e == kDesert || w == kDesert || s == kDesert;
    return center != kForest || !nearPlainsOrDesert ? center : kMountains;
}

// AddClimateLayers.AddSpecialBiomesLayer.
int addSpecialSample(LayerContext& c, int value) {
    if (!isShallowOcean(value) && c.nextInt(13) == 0) {
        value |= (1 + c.nextInt(15)) << 8 & 3840;
    }
    return value;
}

// AddMushroomIslandLayer.
int addMushroomSample(LayerContext& c, int sw, int se, int ne, int nw, int center) {
    return isShallowOcean(center) && isShallowOcean(nw) && isShallowOcean(sw) &&
                   isShallowOcean(ne) && isShallowOcean(se) && c.nextInt(100) == 0
               ? kMushroomFields
               : center;
}

// SimpleLandNoiseLayer.
int simpleLandNoiseSample(LayerContext& c, int value) {
    return isShallowOcean(value) ? value : c.nextInt(299999) + 2;
}

// SetBaseBiomesLayer (legacy flag off).
int setBaseBiomesSample(LayerContext& c, int value) {
    const int special = (value & 3840) >> 8;
    value &= ~3840;
    if (isOcean(value) || value == kMushroomFields) return value;
    switch (value) {
    case kPlains:
        if (special > 0) return c.nextInt(3) == 0 ? kBadlandsPlateau : kWoodedBadlandsPlateau;
        return std::array<int, 6>{kDesert, kDesert, kDesert, kSavanna, kSavanna, kPlains}
            [static_cast<std::size_t>(c.nextInt(6))];
    case kDesert:
        if (special > 0) return kJungle;
        return std::array<int, 6>{kForest, kDarkForest, kMountains, kPlains, kBirchForest, kSwamp}
            [static_cast<std::size_t>(c.nextInt(6))];
    case kMountains:
        if (special > 0) return kGiantTreeTaiga;
        return std::array<int, 4>{kForest, kMountains, kTaiga, kPlains}
            [static_cast<std::size_t>(c.nextInt(4))];
    case kForest:
        return std::array<int, 4>{kSnowyTundra, kSnowyTundra, kSnowyTundra, kSnowyTaiga}
            [static_cast<std::size_t>(c.nextInt(4))];
    default:
        return kMushroomFields;
    }
}

// AddBambooJungleLayer.
int addBambooSample(LayerContext& c, int se) {
    return c.nextInt(10) == 0 && se == kJungle ? kBambooJungle : se;
}

// EaseBiomeEdgeLayer.
int easeEdgeSample(LayerContext& c, int n, int e, int s, int w, int center) {
    // Mountains / badlands-plateau / giant-taiga edge easing collapse onto
    // their base biome in this reduced set, so only the desert-adjacent-snow
    // and swamp rules survive.
    if (center == kDesert && (n == kSnowyTundra || e == kSnowyTundra || w == kSnowyTundra ||
                              s == kSnowyTundra)) {
        return kMountains; // WoodedMountains collapses onto Mountains.
    }
    if (center == kSwamp) {
        if (n == kDesert || e == kDesert || w == kDesert || s == kDesert || n == kSnowyTaiga ||
            e == kSnowyTaiga || w == kSnowyTaiga || s == kSnowyTaiga || n == kSnowyTundra ||
            e == kSnowyTundra || w == kSnowyTundra || s == kSnowyTundra) {
            return kPlains;
        }
        if (n == kJungle || e == kJungle || w == kJungle || s == kJungle || n == kBambooJungle ||
            e == kBambooJungle || w == kBambooJungle || s == kBambooJungle) {
            return kJungleEdge;
        }
    }
    return center;
}

// AddHillsLayer, with hills variants collapsed onto their base biome.
int addHillsSample(LayerContext& c, const CachingSampler& biome, const CachingSampler& noise,
                   int x, int z) {
    const int i = biome.sample(x, z);
    const int j = noise.sample(x, z);
    const int k = (j - 2) % 29;
    if (c.nextInt(3) == 0 || k == 0) {
        int l = i;
        if (i == kDarkForest) l = kPlains;
        else if (i == kPlains) l = kForest;
        else if (i == kOcean) l = kDeepOcean;
        else if (i == kLukewarmOcean) l = kDeepLukewarmOcean;
        else if (i == kColdOcean) l = kDeepColdOcean;
        else if (i == kFrozenOcean) l = kDeepFrozenOcean;
        else if (i == kDeepOcean || i == kDeepLukewarmOcean || i == kDeepColdOcean ||
                 i == kDeepFrozenOcean) {
            if (c.nextInt(3) == 0) l = c.nextInt(2) == 0 ? kPlains : kForest;
        }
        // getModifiedBiome collapses the hills variants; only the savanna
        // plateau keeps a distinct base here.
        if (k == 0 && l != i) {
            if (l == kSavannaPlateau) l = kSavanna;
        }
        if (l != i) {
            int similarNeighbours = 0;
            if (areSimilar(biome.sample(x, z - 1), i)) ++similarNeighbours;
            if (areSimilar(biome.sample(x + 1, z), i)) ++similarNeighbours;
            if (areSimilar(biome.sample(x - 1, z), i)) ++similarNeighbours;
            if (areSimilar(biome.sample(x, z + 1), i)) ++similarNeighbours;
            if (similarNeighbours >= 3) return l;
        }
    }
    return i;
}

// AddSunflowerPlainsLayer.
int addSunflowerSample(LayerContext& c, int se) {
    return c.nextInt(57) == 0 && se == kPlains ? kSunflowerPlains : se;
}

// AddEdgeBiomesLayer.
int addEdgeBiomesSample(LayerContext& c, int n, int e, int s, int w, int center) {
    if (category(center) == Category::Jungle) {
        if (!isWooded(n) || !isWooded(e) || !isWooded(s) || !isWooded(w)) return kJungleEdge;
        if (isOcean(n) || isOcean(e) || isOcean(s) || isOcean(w)) return kBeach;
    } else if (center != kMountains) {
        if (isSnowy(center)) {
            if (!isOcean(center) && (isOcean(n) || isOcean(e) || isOcean(s) || isOcean(w))) {
                return kSnowyBeach;
            }
        } else if (!isBadlandsLike(center)) {
            if (!isOcean(center) && center != kRiver && center != kSwamp &&
                (isOcean(n) || isOcean(e) || isOcean(s) || isOcean(w))) {
                return kBeach;
            }
        }
    } else if (!isOcean(center) && (isOcean(n) || isOcean(e) || isOcean(s) || isOcean(w))) {
        return kStoneShore;
    }
    return center;
}

// NoiseToRiverLayer.
int isValidForRiver(int value) { return value >= 2 ? 2 + (value & 1) : value; }

int noiseToRiverSample(LayerContext& c, int n, int e, int s, int w, int center) {
    const int centreValue = isValidForRiver(center);
    return centreValue == isValidForRiver(w) && centreValue == isValidForRiver(n) &&
                   centreValue == isValidForRiver(e) && centreValue == isValidForRiver(s)
               ? -1
               : kRiver;
}

// SmoothenShorelineLayer.
int smoothenShorelineSample(LayerContext& c, int n, int e, int s, int w, int center) {
    const bool eastWestSame = e == w;
    const bool northSouthSame = n == s;
    if (eastWestSame == northSouthSame) {
        if (eastWestSame) return c.nextInt(2) == 0 ? w : n;
        return center;
    }
    return eastWestSame ? w : n;
}

// AddRiversLayer.
int addRiversSample(LayerContext& c, const CachingSampler& biome, const CachingSampler& river,
                    int x, int z) {
    const int i = biome.sample(x, z);
    const int j = river.sample(x, z);
    if (isOcean(i)) return i;
    if (j == kRiver) return kRiver;
    return i;
}

// ApplyOceanTemperatureLayer.
int applyOceanTemperatureSample(LayerContext& c, const CachingSampler& biome,
                                const CachingSampler& oceanTemp, int x, int z) {
    const int i = biome.sample(x, z);
    const int j = oceanTemp.sample(x, z);
    if (!isOcean(i)) return i;
    for (int m = -8; m <= 8; m += 4) {
        for (int n = -8; n <= 8; n += 4) {
            const int o = biome.sample(x + m, z + n);
            if (!isOcean(o)) {
                if (j == kWarmOcean) return kLukewarmOcean;
                if (j == kFrozenOcean) return kColdOcean;
            }
        }
    }
    if (i == kDeepOcean) {
        if (j == kLukewarmOcean) return kDeepLukewarmOcean;
        if (j == kOcean) return kDeepOcean;
        if (j == kColdOcean) return kDeepColdOcean;
        if (j == kFrozenOcean) return kDeepFrozenOcean;
    }
    return j;
}

// ---------------------------------------------------------------------------
// ScaleLayer#stack: apply `count` copies of the layer to the factory.
[[nodiscard]] Factory stackScale(std::uint64_t seed, Factory factory, int seedBase, int count) {
    for (int i = 0; i < count; ++i) {
        factory = makeParented(makeContext(seed, seedBase + i), std::move(factory),
                               [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                                   return scaleSample(c, parent, x, z, false);
                               });
    }
    return factory;
}

// ---------------------------------------------------------------------------
// The pipeline itself: BiomeLayers#build(old=false, biomeSize=4, riverSize=4),
// then the layer-internal id is mapped onto the registered Biome set.
[[nodiscard]] Factory buildPipeline(std::uint64_t seed) {
    const auto ctx = [seed](std::int64_t salt) { return makeContext(seed, salt); };

    Factory f = makeInit(ctx(1), continentSample);
    f = makeParented(ctx(2000), std::move(f),
                     [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                         return scaleSample(c, parent, x, z, true);
                     });
    f = makeParented(ctx(1), std::move(f),
                     [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                         return increaseEdgeCurvatureSample(
                             c, parent.sample(x - 1, z + 1), parent.sample(x + 1, z + 1),
                             parent.sample(x + 1, z - 1), parent.sample(x - 1, z - 1),
                             parent.sample(x, z));
                     });
    f = makeParented(ctx(2001), std::move(f),
                     [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                         return scaleSample(c, parent, x, z, false);
                     });
    for (const std::int64_t salt : {2, 50, 70}) {
        f = makeParented(ctx(salt), std::move(f),
                         [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                             return increaseEdgeCurvatureSample(
                                 c, parent.sample(x - 1, z + 1), parent.sample(x + 1, z + 1),
                                 parent.sample(x + 1, z - 1), parent.sample(x - 1, z - 1),
                                 parent.sample(x, z));
                         });
    }
    f = makeParented(ctx(2), std::move(f),
                     [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                         return addIslandSample(c, parent.sample(x, z - 1), parent.sample(x + 1, z),
                                                parent.sample(x, z + 1),
                                                parent.sample(x - 1, z), parent.sample(x, z));
                     });

    Factory oceanTemp = makeInit(ctx(2), oceanTemperatureSample);
    oceanTemp = stackScale(seed, std::move(oceanTemp), 2001, 6);

    f = makeParented(ctx(2), std::move(f),
                     [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                         return addColdClimatesSample(c, parent.sample(x, z));
                     });
    f = makeParented(ctx(3), std::move(f),
                     [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                         return increaseEdgeCurvatureSample(
                             c, parent.sample(x - 1, z + 1), parent.sample(x + 1, z + 1),
                             parent.sample(x + 1, z - 1), parent.sample(x - 1, z - 1),
                             parent.sample(x, z));
                     });
    f = makeParented(ctx(2), std::move(f),
                     [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                         return addTemperateSample(c, parent.sample(x, z - 1),
                                                   parent.sample(x + 1, z),
                                                   parent.sample(x, z + 1),
                                                   parent.sample(x - 1, z), parent.sample(x, z));
                     });
    f = makeParented(ctx(2), std::move(f),
                     [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                         return addCoolSample(c, parent.sample(x, z - 1),
                                              parent.sample(x + 1, z), parent.sample(x, z + 1),
                                              parent.sample(x - 1, z), parent.sample(x, z));
                     });
    f = makeParented(ctx(3), std::move(f),
                     [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                         return addSpecialSample(c, parent.sample(x, z));
                     });
    f = makeParented(ctx(2002), std::move(f),
                     [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                         return scaleSample(c, parent, x, z, false);
                     });
    f = makeParented(ctx(2003), std::move(f),
                     [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                         return scaleSample(c, parent, x, z, false);
                     });
    f = makeParented(ctx(4), std::move(f),
                     [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                         return increaseEdgeCurvatureSample(
                             c, parent.sample(x - 1, z + 1), parent.sample(x + 1, z + 1),
                             parent.sample(x + 1, z - 1), parent.sample(x - 1, z - 1),
                             parent.sample(x, z));
                     });
    f = makeParented(ctx(5), std::move(f),
                     [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                         return addMushroomSample(c, parent.sample(x - 1, z + 1),
                                                  parent.sample(x + 1, z + 1),
                                                  parent.sample(x + 1, z - 1),
                                                  parent.sample(x - 1, z - 1),
                                                  parent.sample(x, z));
                     });
    f = makeParented(ctx(4), std::move(f),
                     [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                         return addDeepOceanSample(c, parent.sample(x, z - 1),
                                                   parent.sample(x + 1, z),
                                                   parent.sample(x, z + 1),
                                                   parent.sample(x - 1, z), parent.sample(x, z));
                     });

    Factory landNoise = makeParented(ctx(100), f,
                                     [](LayerContext& c, const CachingSampler& parent, int x,
                                        int z) { return simpleLandNoiseSample(c, parent.sample(x, z)); });

    Factory base = makeParented(ctx(200), f,
                                [](LayerContext& c, const CachingSampler& parent, int x,
                                   int z) { return setBaseBiomesSample(c, parent.sample(x, z)); });
    base = makeParented(ctx(1001), std::move(base),
                        [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                            return addBambooSample(c, parent.sample(x, z));
                        });
    base = stackScale(seed, std::move(base), 1000, 2);
    base = makeParented(ctx(1000), std::move(base),
                        [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                            return easeEdgeSample(c, parent.sample(x, z - 1),
                                                  parent.sample(x + 1, z), parent.sample(x, z + 1),
                                                  parent.sample(x - 1, z), parent.sample(x, z));
                        });

    Factory hillsNoise = stackScale(seed, landNoise, 1000, 2);
    base = makeMerging(ctx(1000), std::move(base), hillsNoise, addHillsSample);

    Factory riverNoise = stackScale(seed, landNoise, 1000, 2);
    riverNoise = stackScale(seed, std::move(riverNoise), 1000, 4);
    riverNoise = makeParented(ctx(1), std::move(riverNoise),
                              [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                                  return noiseToRiverSample(
                                      c, parent.sample(x, z - 1), parent.sample(x + 1, z),
                                      parent.sample(x, z + 1), parent.sample(x - 1, z),
                                      parent.sample(x, z));
                              });
    riverNoise = makeParented(ctx(1000), std::move(riverNoise),
                              [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                                  return smoothenShorelineSample(
                                      c, parent.sample(x, z - 1), parent.sample(x + 1, z),
                                      parent.sample(x, z + 1), parent.sample(x - 1, z),
                                      parent.sample(x, z));
                              });

    base = makeParented(ctx(1001), std::move(base),
                        [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                            return addSunflowerSample(c, parent.sample(x, z));
                        });
    for (int i = 0; i < 4; ++i) {
        base = makeParented(ctx(1000 + i), std::move(base),
                            [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                                return scaleSample(c, parent, x, z, false);
                            });
        if (i == 0) {
            base = makeParented(ctx(3), std::move(base),
                                [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                                    return increaseEdgeCurvatureSample(
                                        c, parent.sample(x - 1, z + 1),
                                        parent.sample(x + 1, z + 1), parent.sample(x + 1, z - 1),
                                        parent.sample(x - 1, z - 1), parent.sample(x, z));
                                });
        }
        if (i == 1) {
            base = makeParented(ctx(1000), std::move(base),
                                [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                                    return addEdgeBiomesSample(
                                        c, parent.sample(x, z - 1), parent.sample(x + 1, z),
                                        parent.sample(x, z + 1), parent.sample(x - 1, z),
                                        parent.sample(x, z));
                                });
        }
    }
    base = makeParented(ctx(1000), std::move(base),
                        [](LayerContext& c, const CachingSampler& parent, int x, int z) {
                            return smoothenShorelineSample(
                                c, parent.sample(x, z - 1), parent.sample(x + 1, z),
                                parent.sample(x, z + 1), parent.sample(x - 1, z),
                                parent.sample(x, z));
                        });
    base = makeMerging(ctx(100), std::move(base), riverNoise, addRiversSample);
    return makeMerging(ctx(100), std::move(base), oceanTemp, applyOceanTemperatureSample);
}

// Maps a layer-internal biome id onto the registered Biome set.
[[nodiscard]] Biome mapBiome(int id) {
    switch (id) {
    case kOcean:
    case kWarmOcean:
    case kLukewarmOcean:
    case kColdOcean:
    case kFrozenOcean:
        return Biome::Ocean;
    case kDeepOcean:
    case kDeepWarmOcean:
    case kDeepLukewarmOcean:
    case kDeepColdOcean:
    case kDeepFrozenOcean:
        return Biome::DeepOcean;
    case kRiver:
    case kFrozenRiver:
        return Biome::River;
    case kPlains:
    case kSunflowerPlains:
    case kMushroomFields:
    case kMushroomShore:
        return Biome::Plains;
    case kDesert:
    case kBadlands:
    case kWoodedBadlandsPlateau:
    case kBadlandsPlateau:
        return Biome::Desert;
    case kMountains:
        return Biome::WindsweptHills;
    case kForest:
        return Biome::Forest;
    case kTaiga:
    case kSnowyTaiga:
    case kGiantTreeTaiga:
        return Biome::Taiga;
    case kSwamp:
        return Biome::Swamp;
    case kSnowyTundra:
        return Biome::SnowyPlains;
    case kBirchForest:
        return Biome::BirchForest;
    case kDarkForest:
        return Biome::DarkForest;
    case kSavanna:
    case kSavannaPlateau:
        return Biome::Savanna;
    case kJungle:
    case kJungleEdge:
    case kBambooJungle:
        return Biome::Jungle;
    case kBeach:
    case kSnowyBeach:
    case kStoneShore:
        return Biome::Beach;
    default:
        return Biome::Ocean;
    }
}

} // namespace

struct LayeredBiomeSource::Impl {
    explicit Impl(std::uint64_t seed) : top(buildPipeline(seed)()) {}
    CachingSampler top;
};

LayeredBiomeSource::LayeredBiomeSource(std::uint64_t seed) : impl_(std::make_unique<Impl>(seed)) {}

LayeredBiomeSource::~LayeredBiomeSource() = default;

Biome LayeredBiomeSource::sample(int quartX, int quartZ) const {
    return mapBiome(impl_->top.sample(quartX, quartZ));
}

} // namespace mc::world::gen
