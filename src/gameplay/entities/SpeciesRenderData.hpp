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

// Loads a species' skin at its model's declared texture size. It first resolves
// the standard textures/<path> ResourceLocation through the active pack stack,
// preserving per-file overlay priority. Project-owned entity art under
// resources/entity/<path> remains a fallback, followed by a procedural texture
// painted through the same boxUvFaceRect mapping.
[[nodiscard]] std::vector<std::uint8_t> buildSpeciesSkin(const std::filesystem::path& resourceRoot,
                                                         const assets::ResourceProvider& resources,
                                                         const animation::SkeletalModel& model,
                                                         std::string_view texturePath,
                                                         const glm::vec2& fallbackSize);

} // namespace mc::gameplay::entities
