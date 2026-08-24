#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::core {
class Json;
}

namespace mc::animation {

// A Bedrock euler rotation (degrees) as a matrix, about the origin. Bedrock
// stacks the axes Z, then Y, then X, i.e. the matrix is Rz * Ry * Rx and a point
// is rotated around X first. Bones, cubes and the offline tools all share this
// one definition — tools/entity_uv_lib.py::rot_matrix and the texture editor's
// rotMatrix() mirror it, so a pose in the editor matches the pose in game.
[[nodiscard]] glm::mat4 rotationMatrix(const glm::vec3& degrees);

// The same rotation applied around `pivot` instead of the origin.
[[nodiscard]] glm::mat4 rotationAboutPivot(const glm::vec3& degrees, const glm::vec3& pivot);

// One box within a bone, in Bedrock model space (16 units per block, Y up).
struct ModelCube final {
    glm::vec3 origin{0.0F};   // minimum corner
    glm::vec3 size{0.0F};     // extent in each axis
    glm::vec2 uv{0.0F};       // top-left UV of the box net, in texels
    glm::vec3 pivot{0.0F};    // per-cube rotation pivot (defaults to origin+size/2)
    glm::vec3 rotation{0.0F}; // per-cube rotation in degrees
    float inflate = 0.0F;     // outward expansion in model units
    bool mirror = false;      // mirror the UV net horizontally
    bool hasRotation = false;

    // Per-face box-UV override (the texture editor's "faces" extension). For each
    // geometric face (0..5 = +X east, -X west, +Y up, -Y down, +Z back, -Z front)
    // the 3-bit net-position rect it samples and a 1-bit 180°-rotation flag, packed
    // as bits [face*4, face*4+4). The identity layout — every face samples its own
    // rect, no rotation — is 0x00543210; the shader unpacks this from
    // textureLayersRotation.w.
    std::uint32_t faceOverride = 0x00543210U;

    // The extent the cube is drawn at. `inflate` grows the box by that many
    // units on every side around its centre; the box-UV net keeps sampling the
    // rects of the *uninflated* `size`, so the same texels stretch over the
    // larger box (Bedrock's overlay/"hat" layers rely on this).
    [[nodiscard]] glm::vec3 renderSize() const { return size + 2.0F * inflate; }
    [[nodiscard]] glm::vec3 center() const { return origin + size * 0.5F; }
};

// One face of a cube's box-UV unwrap, as a texel-space rectangle in the entity
// texture. `face` uses the renderer's cuboid face order: 0 +X, 1 -X, 2 +Y,
// 3 -Y, 4 +Z, 5 -Z.
struct BoxUvRect final {
    glm::vec2 origin{0.0F}; // top-left corner, in texels
    glm::vec2 size{0.0F};   // width/height, in texels
};

// The standard Minecraft/Bedrock box-UV layout for one cube face. Given the
// cube's net origin `uv` (top-left, texels) and its `size` (sx,sy,sz, texels),
// returns the texel rectangle that face samples from. The GLSL entity shader
// mirrors this exact formula, and the procedural entity skin is painted through
// it, so texture and geometry always agree. See box_uv_test.cpp.
[[nodiscard]] BoxUvRect boxUvFaceRect(int face, glm::vec2 uv, glm::vec3 size);

// A single bone: a named, optionally parented transform plus its cubes.
struct ModelBone final {
    std::string name;
    int parent = -1;          // index into SkeletalModel::bones, or -1 for a root
    glm::vec3 pivot{0.0F};    // rotation pivot in model space
    glm::vec3 rotation{0.0F}; // default (rest) rotation in degrees
    bool neverRender = false; // Bedrock `neverRender`: transform-only helper bone
    std::vector<ModelCube> cubes;
};

// A skeletal model loaded from a Bedrock `geometry` document. The same type
// describes blocks (a chest), the player, NPCs and mobs — the geometry file is
// the single source of truth for their shape and bone hierarchy.
class SkeletalModel final {
  public:
    SkeletalModel() = default;

    [[nodiscard]] const std::string& identifier() const { return identifier_; }
    [[nodiscard]] int textureWidth() const { return textureWidth_; }
    [[nodiscard]] int textureHeight() const { return textureHeight_; }
    [[nodiscard]] const std::vector<ModelBone>& bones() const { return bones_; }
    [[nodiscard]] std::size_t boneCount() const { return bones_.size(); }

    // Returns the bone index for a name, or -1 if absent.
    [[nodiscard]] int findBone(std::string_view name) const;

    // Parses a Bedrock `minecraft:geometry` document. When `identifier` is
    // empty the first geometry entry is used; otherwise the matching entry is
    // selected. Throws std::runtime_error on malformed input.
    [[nodiscard]] static SkeletalModel loadGeometry(const core::Json& document,
                                                    std::string_view identifier = {});

    // Convenience: parse raw JSON text and load the geometry.
    [[nodiscard]] static SkeletalModel parse(std::string_view jsonText,
                                             std::string_view identifier = {});

    // Assembles a model directly from a ready bone list (already in rebedrock
    // model space). Parent references are resolved by name against declaration
    // order. This is the programmatic (builder/baker) counterpart to
    // loadGeometry — see PartDefinition.hpp — and applies the same validation
    // (unique names, resolvable parents). Throws std::runtime_error on
    // malformed input.
    [[nodiscard]] static SkeletalModel assemble(std::string identifier, int textureWidth,
                                                int textureHeight, std::vector<ModelBone> bones);

  private:
    std::string identifier_;
    int textureWidth_ = 16;
    int textureHeight_ = 16;
    std::vector<ModelBone> bones_;
    std::unordered_map<std::string, int> boneIndex_;
};

} // namespace mc::animation
