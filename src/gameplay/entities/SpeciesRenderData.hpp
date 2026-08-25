#pragma once

#include "animation/AnimationAssets.hpp"
#include "gameplay/entities/EntityType.hpp"

#include <cstdint>
#include <filesystem>
#include <glm/vec2.hpp>
#include <string_view>
#include <vector>

namespace mc::assets {
class ResourceProvider;
}

namespace mc::gameplay::entities {

class EntityTypeRegistry;

// One species the renderer has bound: its registered EntityType (the key the
// simulation hands the renderer per entity), the geometry + animations loaded
// for it, and its layer in the entity texture array. Built by
// buildSpeciesModels in registry order, so a species' layer index is the index
// of its EntityType in the registry.
struct SpeciesRenderModel final {
    const EntityType* type = nullptr;
    animation::AnimatedModel model;
    float textureLayer = 0.0F;
    // The species' optional second entity-texture-array layer (the sheep fleece),
    // or -1 when it has no secondary skin. Populated by createEntityTextureArray
    // when render().secondaryTexturePath is set and loads.
    float secondaryTextureLayer = -1.0F;
    // Per-bone texture-array layer, indexed by bone order. Precomputed once at
    // load time (a "wool"-prefixed bone resolves to secondaryTextureLayer, every
    // other bone to textureLayer) so the draw loop never re-tests bone names.
    std::vector<float> boneTextureLayer;
    bool loaded = false;
};

// Builds one SpeciesRenderModel per registered species that carries a render
// descriptor. A species' geometry/animations start from its compiled-in builtin
// (so it renders even when resources/animation/ is absent), then prefer the
// on-disk resources/animation/*.json when present. Registering an EntityType
// with a render descriptor is enough for it to be picked up — no renderer code
// needs to change when a new creature is added.
[[nodiscard]] std::vector<SpeciesRenderModel>
buildSpeciesModels(const std::filesystem::path& resourceRoot, const EntityTypeRegistry& registry);

// The box-UV coordinate space for a model: its declared texture_width/height.
// Falls back to `fallbackSize` when a geometry omits the declaration, so the
// net stays 1:1 with the loaded skin instead of collapsing onto the loader's
// 16x16 default.
[[nodiscard]] glm::vec2 entityTextureSize(const animation::SkeletalModel& model,
                                          const glm::vec2& fallbackSize);

// Loads a species' skin at its model's declared texture size, resolving the
// standard textures/<path> ResourceLocation through the active pack stack so
// per-file overlay priority is preserved.
//
// There are exactly two sources, and deliberately so: the pack, or a procedural
// texture painted through the same boxUvFaceRect mapping. There used to be a
// third — a bundled PNG under resources/entity/<path> — but the only file that
// ever lived there was converted from Mojang's zombie skin, which made every
// release carry derived vanilla art while claiming to ship none. A missing skin
// must degrade to the generated placeholder, never to something the project has
// no right to redistribute.
[[nodiscard]] std::vector<std::uint8_t> buildSpeciesSkin(const assets::ResourceProvider& resources,
                                                         const animation::SkeletalModel& model,
                                                         std::string_view texturePath,
                                                         const glm::vec2& fallbackSize);

} // namespace mc::gameplay::entities
