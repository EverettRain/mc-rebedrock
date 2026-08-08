#include "world/gen/BiomeSource.hpp"

#include "world/gen/LayeredBiomeSource.hpp"

namespace mc::world::gen {

BiomeSource::BiomeSource(std::uint64_t seed) : layered_(std::make_unique<LayeredBiomeSource>(seed)) {}

BiomeSource::~BiomeSource() = default;

Biome BiomeSource::biomeForNoiseGeneration(int quartX, int quartZ) const {
    return layered_->sample(quartX, quartZ);
}

} // namespace mc::world::gen
