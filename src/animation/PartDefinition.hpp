#pragma once

#include "animation/SkeletalModel.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <string_view>
#include <vector>

// A C++ mirror of vanilla's client-side geometry builders
// (net.minecraft.client.model.geom.builders.{CubeListBuilder,PartDefinition} and
// net.minecraft.client.model.geom.PartPose) plus a one-shot baker that turns the
// Java model space (Y-down, part-local cube coordinates, texture offsets) into
// rebedrock's Bedrock model space (Y-up, absolute cube origins, box-UV net) and
// emits a `SkeletalModel`.
//
// The point of RN-0a: a *programmatic* replacement for hand-written geo.json.
// Transcribe vanilla `createBodyLayer()` once, bake to the same `SkeletalModel`
// the geo.json path produces, and never interpret the definition at run time
// (bake-not-parse). This file is geometry-definition only: it does not touch the
// animation runtime and does not touch `boxUvFaceRect`.
//
// Coordinate transform (Java -> rebedrock), proven byte-equal against today's
// geo.json for every non-rotated bone of cow (head + four legs):
//   * A part's pose offset in Java is Y-down; rebedrock is Y-up. The whole model
//     mirrors across y = kModelHeight/2, so pivotY_rebedrock = kModelHeight - poseY.
//   * A cube's box is declared part-local as (x0,y0,z0,w,h,d) with y0 the Y-down
//     minimum corner. Its rebedrock (Y-up) minimum corner is
//       origin = (poseX + x0, kModelHeight - (poseY + y0 + h), poseZ + z0),
//     and its extent (w,h,d) is unchanged.
//   * Texture offsets and box-UV are the *current* rebedrock convention (front =
//     +Z, current up/down caps). RN-0a deliberately does not touch the cap
//     convention; aligning to vanilla caps is RN-0b (an atomic global visual
//     change gated on Mac).

namespace mc::animation {

// Java's y=0 plane sits kModelHeight below rebedrock's y=0; a mob model is
// mirrored across this height when moving between the two spaces. 24 (= 1.5
// blocks * 16 units) is the standard Minecraft mob model height Mojang builds
// quadruped/biped poses around (18 - legSize, 24 - legSize, etc).
inline constexpr float kModelHeight = 24.0F;

// Vanilla CubeDeformation: an outward grow applied on every side. rebedrock's
// ModelCube carries a single scalar `inflate`; the builder keeps the vanilla
// spelling so transcription reads 1:1.
struct CubeDeformation final {
    float grow = 0.0F;
    constexpr CubeDeformation() = default;
    constexpr explicit CubeDeformation(float g) : grow(g) {}
    static const CubeDeformation kNone;
};
inline constexpr CubeDeformation CubeDeformation::kNone{};

// One relabel/rotate entry of the box-UV net (the texture editor's "faces"
// extension), used verbatim when a transcribed part reproduces an authored
// geo.json cube. `pos` and `target` are box-UV face indices
// (0..5 = +X,-X,+Y,-Y,+Z,-Z); `rotate180` toggles the 180-degree sample flag.
struct FaceRelabel final {
    int target = 0;
    int pos = 0;
    bool rotate180 = false;
};

// Mirror of vanilla CubeListBuilder. `create().texOffs(u,v).addBox(...)`. Cubes
// are declared in Java model space; the baker performs the Y-flip.
class CubeListBuilder final {
  public:
    static CubeListBuilder create() { return {}; }

    CubeListBuilder& texOffs(int u, int v) {
        texU_ = u;
        texV_ = v;
        return *this;
    }

    CubeListBuilder& mirror(bool value = true) {
        mirror_ = value;
        return *this;
    }

    // Vanilla addBox: part-local Java-space box.
    CubeListBuilder& addBox(float x0, float y0, float z0, float w, float h, float d,
                            const CubeDeformation& grow = CubeDeformation::kNone);

    // Escape hatch for transcribing an *authored* geo.json cube whose stored
    // representation is not a plain Java transform (e.g. cow's rotation-folded,
    // face-compensated body). Values are already in rebedrock/Bedrock model
    // space; the baker copies them through unchanged. This preserves byte
    // equality with today's geo.json without changing any visuals — the
    // compensation itself belongs to RN-0b.
    CubeListBuilder& addBakedCube(glm::vec3 origin, glm::vec3 size, glm::vec2 uv, int texU,
                                  int texV, bool mirror = false, float inflate = 0.0F,
                                  const std::vector<FaceRelabel>& faces = {});

  private:
    friend class PartDefinition;

    struct Entry final {
        // Java-space box (used when `baked` is false).
        glm::vec3 box{0.0F};   // (x0, y0, z0) part-local Java minimum corner
        glm::vec3 size{0.0F};  // (w, h, d)
        glm::vec2 uv{0.0F};    // texel offset (u, v)
        float inflate = 0.0F;
        bool mirror = false;
        // Baked (rebedrock-space) fields, used when `baked` is true.
        bool baked = false;
        glm::vec3 bakedOrigin{0.0F};
        std::vector<FaceRelabel> faces;
    };

    int texU_ = 0;
    int texV_ = 0;
    bool mirror_ = false;
    std::vector<Entry> cubes_;
};

// Mirror of vanilla PartPose: a Java-space offset plus a rest rotation (degrees).
struct PartPose final {
    glm::vec3 offset{0.0F};
    glm::vec3 rotation{0.0F}; // degrees, Bedrock Z,Y,X order (matches ModelBone)

    static PartPose offsetPose(float x, float y, float z) { return {{x, y, z}, {0.0F, 0.0F, 0.0F}}; }
    static PartPose rotationPose(float rx, float ry, float rz) {
        return {{0.0F, 0.0F, 0.0F}, {rx, ry, rz}};
    }
    static PartPose offsetAndRotation(float x, float y, float z, float rx, float ry, float rz) {
        return {{x, y, z}, {rx, ry, rz}};
    }
};

// Mirror of vanilla PartDefinition: the mesh tree. `addOrReplaceChild` appends a
// named bone; pass `parent` (default the root) to reproduce a geo.json parent
// chain. `bake` performs the one-shot Java -> rebedrock transform.
class PartDefinition final {
  public:
    PartDefinition() = default;

    // Adds a bone. `parent` is a previously added bone's name, or empty for a
    // root bone (matching SkeletalModel's -1 parent).
    PartDefinition& addOrReplaceChild(std::string name, const CubeListBuilder& cubes,
                                      const PartPose& pose, std::string_view parent = {});

    // One-shot baker: transcribes the whole tree into a SkeletalModel in
    // rebedrock model space. Nothing is interpreted after this returns.
    [[nodiscard]] SkeletalModel bake(std::string identifier, int textureWidth,
                                     int textureHeight) const;

  private:
    struct BoneDef final {
        std::string name;
        std::string parent;
        PartPose pose;
        CubeListBuilder cubes;
    };
    std::vector<BoneDef> bones_;
};

} // namespace mc::animation
