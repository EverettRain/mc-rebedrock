#include "world/gen/BiomeSource.hpp"

#include "world/gen/LayeredBiomeSource.hpp"
#include "world/gen/MultiNoiseBiomeSource.hpp"

namespace mc::world::gen {

BiomeSource::BiomeSource(std::uint64_t seed)
    : kind_(Kind::Layered), layered_(std::make_unique<LayeredBiomeSource>(seed)) {}

BiomeSource::BiomeSource(NetherTag, std::uint64_t seed)
    : kind_(Kind::MultiNoise), multiNoise_(std::make_unique<MultiNoiseBiomeSource>(seed)) {}

BiomeSource BiomeSource::nether(std::uint64_t seed) { return BiomeSource{NetherTag{}, seed}; }

BiomeSource::BiomeSource(BiomeSource&&) noexcept = default;
BiomeSource& BiomeSource::operator=(BiomeSource&&) noexcept = default;

BiomeSource::~BiomeSource() = default;

Biome BiomeSource::biomeForNoiseGeneration(int quartX, int quartZ) const {
    // One branch on the stored kind, not a virtual dispatch: the nether and the
    // overworld read the same accessor, so NoiseChunkGenerator/Features never
    // learn which dimension they are generating.
    switch (kind_) {
    case Kind::Layered:
        return layered_->sample(quartX, quartZ);
    case Kind::MultiNoise:
        return multiNoise_->sample(quartX, quartZ);
    }
    return Biome::Plains;
}

} // namespace mc::world::gen
