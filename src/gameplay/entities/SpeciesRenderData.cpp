#include "gameplay/entities/SpeciesRenderData.hpp"

#include "assets/ImageData.hpp"
#include "gameplay/entities/EntityRegistry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mc::gameplay::entities {
namespace {

// Built-in model and clips per species, compiled in so world entities work even
// when the resource files under resources/animation/ are missing or were not
// staged (the runtime still prefers the on-disk files when present). Keep these
// in sync with resources/animation/<species>.*.json.
struct BuiltinModel final {
    std::string_view geometry;
    std::string_view animation;
};

// Built-in pig model, compiled in so world entities work even when the resource
// files under resources/animation/ are missing or were not staged (the runtime
// still prefers the on-disk resources/animation/pig.*.json when present). Keep
// these in sync with those files.
constexpr const char* kBuiltinPigGeometry = R"({
  "format_version": "1.12.0",
  "minecraft:geometry": [
    { "description": {"identifier": "geometry.pig", "texture_width": 64, "texture_height": 32},
      "bones": [
        {"name": "body", "pivot": [0,8,2], "rotation": [90,0,0],
         "cubes": [{"origin": [-5,0,-4], "size": [10,16,8], "uv": [28,8],
                    "faces": {"front": {"as": "back"}, "back": {"as": "front"},
                              "up": {"as": "down"}, "down": {"as": "up", "rotate": 180}}}]},
        {"name": "head", "pivot": [0,12,-6],
         "cubes": [{"origin": [-4,8,-14], "size": [8,8,8], "uv": [0,0]},
                   {"origin": [-2,9,-15], "size": [4,3,1], "uv": [16,16]}]},
        {"name": "legFrontRight", "pivot": [-3,6,-5], "cubes": [{"origin": [-5,0,-7], "size": [4,6,4], "uv": [0,16]}]},
        {"name": "legFrontLeft",  "pivot": [3,6,-5],  "cubes": [{"origin": [1,0,-7],  "size": [4,6,4], "uv": [0,16]}]},
        {"name": "legBackRight",  "pivot": [-3,6,7],  "cubes": [{"origin": [-5,0,5],  "size": [4,6,4], "uv": [0,16]}]},
        {"name": "legBackLeft",   "pivot": [3,6,7],   "cubes": [{"origin": [1,0,5],   "size": [4,6,4], "uv": [0,16]}]}
      ]
    }
  ]
})";
constexpr const char* kBuiltinPigAnimation = R"({
  "format_version": "1.8.0",
  "animations": {
    "animation.pig.walk": {"loop": true, "animation_length": 1.5, "bones": {
      "legFrontRight": {"rotation": ["math.cos(query.anim_time * 360 * 0.6662 + 180) * 80.2 * variable.walk_amount", 0, 0]},
      "legFrontLeft":  {"rotation": ["math.cos(query.anim_time * 360 * 0.6662) * 80.2 * variable.walk_amount", 0, 0]},
      "legBackRight":  {"rotation": ["math.cos(query.anim_time * 360 * 0.6662) * 80.2 * variable.walk_amount", 0, 0]},
      "legBackLeft":   {"rotation": ["math.cos(query.anim_time * 360 * 0.6662 + 180) * 80.2 * variable.walk_amount", 0, 0]}
    }},
    "animation.pig.idle": {"loop": true, "animation_length": 4.0, "bones": {
      "head": {"rotation": ["math.cos(query.anim_time * 90) * 4", "math.sin(query.anim_time * 60) * 6", 0]}
    }}
  }
})";

// Built-in cow model and clips, compiled in for the same reason the pig's are:
// the species renders even when resources/animation/cow.*.json are not staged.
// Keep these in sync with those files. The geometry is the vanilla 1.16.1
// CowEntityModel/QuadrupedEntityModel(12) ported to box-UV (64x32): the torso is
// the cow's own 12x18x10 (UV 18,4, not the pig's 10x16x8), laid flat through a
// [90,0,0] bone rotation. The body's face overrides (front<->back, up<->down,
// like the pig's) are load-bearing, not cosmetic: this engine renders models
// Y-up with no flip, while vanilla renders the same torso in a Y-flipped frame,
// so the box-UV face that ends up on the cow's back here is the rect vanilla
// draws on its belly. Re-routing front<->back (and up<->down, whose cap naming
// is also opposite the vanilla Cuboid's) reproduces the vanilla texture layout.
// Horns and udders are the cow's extra parts. The head (8x8x6, UV 0,0) sits on
// top at world y 16..24 like the vanilla model.
constexpr const char* kBuiltinCowGeometry = R"({
  "format_version": "1.12.0",
  "minecraft:geometry": [
    { "description": {"identifier": "geometry.cow", "texture_width": 64, "texture_height": 32},
      "bones": [
        {"name": "body", "pivot": [0,19,2], "rotation": [90,0,0],
         "cubes": [{"origin": [-6,9,-1], "size": [12,18,10], "uv": [18,4],
                    "faces": {"front": {"as": "back"}, "back": {"as": "front"},
                              "up": {"as": "down"}, "down": {"as": "up", "rotate": 180}}}]},
        {"name": "udder", "parent": "body", "pivot": [0,19,2],
         "cubes": [{"origin": [-2,21,9], "size": [4,6,1], "uv": [52,0]}]},
        {"name": "head", "pivot": [0,20,-8],
         "cubes": [{"origin": [-4,16,-14], "size": [8,8,6], "uv": [0,0]}]},
        {"name": "hornLeft", "parent": "head", "pivot": [-4.5,24,-11.5],
         "cubes": [{"origin": [-5,22,-12], "size": [1,3,1], "uv": [22,0]}]},
        {"name": "hornRight", "parent": "head", "pivot": [4.5,24,-11.5],
         "cubes": [{"origin": [4,22,-12], "size": [1,3,1], "uv": [22,0]}]},
        {"name": "legFrontRight", "pivot": [-4,12,-6], "cubes": [{"origin": [-6,0,-8], "size": [4,12,4], "uv": [0,16]}]},
        {"name": "legFrontLeft",  "pivot": [4,12,-6],  "cubes": [{"origin": [2,0,-8],  "size": [4,12,4], "uv": [0,16]}]},
        {"name": "legBackRight",  "pivot": [-4,12,7],  "cubes": [{"origin": [-6,0,5],  "size": [4,12,4], "uv": [0,16]}]},
        {"name": "legBackLeft",   "pivot": [4,12,7],   "cubes": [{"origin": [2,0,5],   "size": [4,12,4], "uv": [0,16]}]}
      ]
    }
  ]
})";
constexpr const char* kBuiltinCowAnimation = R"({
  "format_version": "1.8.0",
  "animations": {
    "animation.cow.walk": {"loop": true, "animation_length": 1.5, "bones": {
      "legFrontRight": {"rotation": ["math.cos(query.anim_time * 360 * 0.6662 + 180) * 80.2 * variable.walk_amount", 0, 0]},
      "legFrontLeft":  {"rotation": ["math.cos(query.anim_time * 360 * 0.6662) * 80.2 * variable.walk_amount", 0, 0]},
      "legBackRight":  {"rotation": ["math.cos(query.anim_time * 360 * 0.6662) * 80.2 * variable.walk_amount", 0, 0]},
      "legBackLeft":   {"rotation": ["math.cos(query.anim_time * 360 * 0.6662 + 180) * 80.2 * variable.walk_amount", 0, 0]}
    }},
    "animation.cow.idle": {"loop": true, "animation_length": 4.0, "bones": {
      "head": {"rotation": ["math.cos(query.anim_time * 90) * 4", "math.sin(query.anim_time * 60) * 6", 0]}
    }}
  }
})";

// Built-in zombie model, compiled in the same way as the pig so the hostile
// mob renders even when the resource files are missing. Keep these in sync with
// resources/animation/zombie.*.json. The geometry is the standard Bedrock
// box-UV humanoid (64x64): the left arm/leg mirror the right limbs' texture,
// matching the classic Java zombie skin the box-UV texture is converted from.
constexpr const char* kBuiltinZombieGeometry = R"({
  "format_version": "1.12.0",
  "minecraft:geometry": [
    { "description": {"identifier": "geometry.zombie", "texture_width": 64, "texture_height": 64},
      "bones": [
        {"name": "body", "pivot": [0,24,0],
         "cubes": [{"origin": [-4,12,-2], "size": [8,12,4], "uv": [16,16]}]},
        {"name": "head", "parent": "body", "pivot": [0,24,0],
         "cubes": [{"origin": [-4,24,-4], "size": [8,8,8], "uv": [0,0]}]},
        {"name": "rightArm", "parent": "body", "pivot": [-5,22,0],
         "cubes": [{"origin": [-8,12,-2], "size": [4,12,4], "uv": [40,16]}]},
        {"name": "leftArm", "parent": "body", "pivot": [5,22,0],
         "cubes": [{"origin": [4,12,-2], "size": [4,12,4], "uv": [40,16], "mirror": true}]},
        {"name": "rightLeg", "pivot": [-1.9,12,0],
         "cubes": [{"origin": [-3.9,0,-2], "size": [4,12,4], "uv": [0,16]}]},
        {"name": "leftLeg", "pivot": [1.9,12,0],
         "cubes": [{"origin": [-0.1,0,-2], "size": [4,12,4], "uv": [0,16], "mirror": true}]}
      ]
    }
  ]
})";
constexpr const char* kBuiltinZombieAnimation = R"({
  "format_version": "1.8.0",
  "animations": {
    "animation.zombie.walk": {"loop": true, "animation_length": 1.0, "bones": {
      "rightLeg": {"rotation": ["math.cos(query.anim_time * 360) * 40 * variable.walk_amount", 0, 0]},
      "leftLeg":  {"rotation": ["math.cos(query.anim_time * 360 + 180) * 40 * variable.walk_amount", 0, 0]}
    }},
    "animation.zombie.idle": {"loop": true, "animation_length": 3.0, "bones": {
      "head": {"rotation": ["math.cos(query.anim_time * 90) * 3", "math.sin(query.anim_time * 60) * 5", 0]},
      "body": {"rotation": ["math.sin(query.anim_time * 90) * 1", 0, 0]},
      "rightArm": {"rotation": [90, 0, 0]},
      "leftArm":  {"rotation": [90, 0, 0]}
    }}
  }
})";

// The compiled-in model for a species, keyed by its identifier path ("pig",
// "cow", "zombie"). A species without an entry still renders from its on-disk
// resources/animation/*.json when those are present.
[[nodiscard]] const BuiltinModel* builtinFor(std::string_view speciesPath) {
    static constexpr BuiltinModel kPig{kBuiltinPigGeometry, kBuiltinPigAnimation};
    static constexpr BuiltinModel kCow{kBuiltinCowGeometry, kBuiltinCowAnimation};
    static constexpr BuiltinModel kZombie{kBuiltinZombieGeometry, kBuiltinZombieAnimation};
    static constexpr std::array<std::pair<std::string_view, const BuiltinModel*>, 3> kTable{{
        {"pig", &kPig},
        {"cow", &kCow},
        {"zombie", &kZombie},
    }};
    for (const auto& [path, model] : kTable) {
        if (path == speciesPath) {
            return model;
        }
    }
    return nullptr;
}

} // namespace

std::vector<SpeciesRenderModel> buildSpeciesModels(const std::filesystem::path& resourceRoot,
                                                   const EntityTypeRegistry& registry) {
    std::vector<SpeciesRenderModel> result;
    result.reserve(registry.size());
    for (const EntityType* type : registry.all()) {
        const auto& render = type->render();
        if (render.geometryPath.empty()) {
            // Not a renderable species; the renderer has no model to bind.
            continue;
        }
        SpeciesRenderModel species;
        species.type = type;
        species.textureLayer = static_cast<float>(result.size());
        const BuiltinModel* builtin = builtinFor(type->id().path);
        if (builtin != nullptr) {
            try {
                species.model.model = animation::SkeletalModel::parse(builtin->geometry);
                species.model.animations = animation::AnimationLibrary::parse(builtin->animation);
                species.loaded = species.model.model.boneCount() > 0U;
            } catch (const std::exception& exception) {
                std::cerr << "Built-in " << type->id().path << " model failed to parse: "
                          << exception.what() << '\n';
                species.loaded = false;
            }
        }
        if (species.loaded || builtin == nullptr) {
            // Prefer the on-disk assets when present. A species with no
            // compiled-in builtin still loads here, so new creatures need no
            // renderer change.
            try {
                auto disk = animation::loadAnimatedModel(
                    resourceRoot / std::string{render.geometryPath},
                    {resourceRoot / std::string{render.animationPath}});
                if (disk.model.boneCount() > 0U) {
                    species.model = std::move(disk);
                    species.loaded = true;
                }
            } catch (const std::exception&) {
                // Keep the built-in model (or leave the species unloaded when
                // neither a builtin nor disk assets exist).
            }
        }
        result.push_back(std::move(species));
    }
    return result;
}

glm::vec2 entityTextureSize(const animation::SkeletalModel& model, const glm::vec2& fallbackSize) {
    const auto declared = glm::vec2{static_cast<float>(model.textureWidth()),
                                    static_cast<float>(model.textureHeight())};
    if (declared.x > 0.0F && declared.y > 0.0F) {
        return declared;
    }
    return glm::max(fallbackSize, glm::vec2{1.0F});
}

std::vector<std::uint8_t> buildSpeciesSkin(const std::filesystem::path& resourceRoot,
                                           const std::filesystem::path& vanillaTexturesRoot,
                                           const animation::SkeletalModel& model,
                                           std::string_view texturePath,
                                           const glm::vec2& fallbackSize) {
    const glm::vec2 declared = entityTextureSize(model, fallbackSize);
    const std::uint32_t width = std::max(1U, static_cast<std::uint32_t>(declared.x));
    const std::uint32_t height = std::max(1U, static_cast<std::uint32_t>(declared.y));
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4U, 0U);
    const auto loadInto = [&](const std::filesystem::path& path) -> bool {
        try {
            auto image = assets::ImageData::loadRgba(path);
            if (image.width > 0 && image.height > 0) {
                const auto imageWidth = static_cast<std::uint32_t>(image.width);
                const auto imageHeight = static_cast<std::uint32_t>(image.height);
                const std::uint32_t copyW = std::min(width, imageWidth);
                const std::uint32_t copyH = std::min(height, imageHeight);
                for (std::uint32_t y = 0; y < copyH; ++y) {
                    for (std::uint32_t x = 0; x < copyW; ++x) {
                        const std::size_t src =
                            (static_cast<std::size_t>(y) * imageWidth + x) * 4U;
                        const std::size_t dst =
                            (static_cast<std::size_t>(y) * width + x) * 4U;
                        std::memcpy(&rgba[dst], &image.rgba[src], 4U);
                    }
                }
                std::cout << "Loaded " << texturePath << " skin " << copyW << 'x' << copyH
                          << '\n';
                return true;
            }
        } catch (const std::exception&) {
            // Try the next source.
        }
        return false;
    };
    if (loadInto(resourceRoot / "entity" / std::string{texturePath})) {
        return rgba;
    }
    if (loadInto(vanillaTexturesRoot / std::string{texturePath})) {
        return rgba;
    }
    // Procedural fallback: the atlas *is* the declared box-UV space, so
    // paint one texel per declared texel and fill in each face rect —
    // the model still reads without the .png, through the very same
    // mapping. Nets that escape the declaration are clipped, which is
    // exactly what a real skin would show (the sampler repeats).
    const auto fillRect = [&](const animation::BoxUvRect& rect,
                              std::array<std::uint8_t, 3> base,
                              std::array<std::uint8_t, 3> border) {
        const int x0 = static_cast<int>(std::lround(rect.origin.x));
        const int y0 = static_cast<int>(std::lround(rect.origin.y));
        const int x1 = static_cast<int>(std::lround(rect.origin.x + rect.size.x));
        const int y1 = static_cast<int>(std::lround(rect.origin.y + rect.size.y));
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (x < 0 || y < 0 || x >= static_cast<int>(width) ||
                    y >= static_cast<int>(height)) {
                    continue;
                }
                const bool edge = x == x0 || x == x1 - 1 || y == y0 || y == y1 - 1;
                const auto& c = edge ? border : base;
                const std::size_t i =
                    (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4U;
                rgba[i] = c[0];
                rgba[i + 1] = c[1];
                rgba[i + 2] = c[2];
                rgba[i + 3] = 255U;
            }
        }
    };
    for (const auto& bone : model.bones()) {
        std::array<std::uint8_t, 3> base{230U, 155U, 160U};
        std::array<std::uint8_t, 3> border{180U, 110U, 115U};
        if (bone.name == "head") {
            base = {240U, 175U, 178U};
        } else if (bone.name.rfind("leg", 0) == 0) {
            base = {190U, 120U, 125U};
            border = {150U, 90U, 95U};
        }
        for (const auto& cube : bone.cubes) {
            for (int face = 0; face < 6; ++face) {
                fillRect(animation::boxUvFaceRect(face, cube.uv, cube.size), base, border);
            }
        }
    }
    return rgba;
}

} // namespace mc::gameplay::entities
