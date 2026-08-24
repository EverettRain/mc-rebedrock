#include "animation/PartDefinition.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace mc::animation {

CubeListBuilder& CubeListBuilder::addBox(float x0, float y0, float z0, float w, float h, float d,
                                         const CubeDeformation& grow) {
    Entry entry;
    entry.box = {x0, y0, z0};
    entry.size = {w, h, d};
    entry.uv = {static_cast<float>(texU_), static_cast<float>(texV_)};
    entry.inflate = grow.grow;
    entry.mirror = mirror_;
    entry.baked = false;
    cubes_.push_back(std::move(entry));
    return *this;
}

CubeListBuilder& CubeListBuilder::addBakedCube(glm::vec3 origin, glm::vec3 size, glm::vec2 uv,
                                               int texU, int texV, bool mirror, float inflate,
                                               const std::vector<FaceRelabel>& faces) {
    Entry entry;
    entry.baked = true;
    entry.bakedOrigin = origin;
    entry.size = size;
    entry.uv = uv;
    entry.inflate = inflate;
    entry.mirror = mirror;
    entry.faces = faces;
    // texOffs is carried on the builder too, but a baked cube states its own uv
    // directly; texU/texV are accepted for symmetry with addBox and ignored when
    // they agree with `uv` (they always do at the call sites in this codebase).
    (void)texU;
    (void)texV;
    cubes_.push_back(std::move(entry));
    return *this;
}

PartDefinition& PartDefinition::addOrReplaceChild(std::string name, const CubeListBuilder& cubes,
                                                  const PartPose& pose, std::string_view parent) {
    BoneDef bone;
    bone.name = std::move(name);
    bone.parent = std::string{parent};
    bone.pose = pose;
    bone.cubes = cubes;
    bones_.push_back(std::move(bone));
    return *this;
}

namespace {

// The identity box-UV face packing: every geometric face samples its own net
// rect with no rotation. Must match ModelCube::faceOverride's default and
// loadGeometry's packing exactly.
constexpr std::uint32_t kIdentityFaceOverride = 0x00543210U;

// Applies one relabel entry the same way loadGeometry does: the nibble at
// `target*4` holds the low-3-bit source position and a bit-3 180-degree flag.
[[nodiscard]] std::uint32_t applyRelabel(std::uint32_t packed, const FaceRelabel& relabel) {
    const auto target = static_cast<std::uint32_t>(relabel.target);
    const auto pos = static_cast<std::uint32_t>(relabel.pos);
    const std::uint32_t mask = 0xFU << (target * 4U);
    packed &= ~mask;
    packed |= (pos & 0x7U) << (target * 4U);
    if (relabel.rotate180) {
        packed |= (1U << (target * 4U + 3U));
    }
    return packed;
}

} // namespace

SkeletalModel PartDefinition::bake(std::string identifier, int textureWidth,
                                   int textureHeight) const {
    std::vector<ModelBone> bones;
    bones.reserve(bones_.size());

    // First pass by index: resolve parent names against declaration order so a
    // child can name a parent declared later, matching loadGeometry.
    const auto findBoneIndex = [this](std::string_view name) -> int {
        if (name.empty()) {
            return -1;
        }
        for (std::size_t i = 0U; i < bones_.size(); ++i) {
            if (bones_[i].name == name) {
                return static_cast<int>(i);
            }
        }
        throw std::runtime_error("geometry '" + std::string{name} +
                                 "' referenced as parent but not declared");
    };

    for (const BoneDef& def : bones_) {
        ModelBone bone;
        bone.name = def.name;
        bone.parent = findBoneIndex(def.parent);

        // Pose offset: Java -> rebedrock via the full vanilla scale(-1,-1,1)
        // (X and Y flipped; see docs RN-0c). Pivot is a point, so mirror X and Y.
        bone.pivot = {-def.pose.offset.x, kModelHeight - def.pose.offset.y, def.pose.offset.z};
        // Rest rotation conjugated by scale(-1,-1,1) = diag(-1,-1,1). Because the
        // baked geometry is pre-flipped, the runtime rotation must be S*R*S so that
        // R'*(S*v) == S*(R*v). For the Z,Y,X euler that maps (rx,ry,rz) -> (-rx,
        // -ry, rz): a +90 X body pose (Java) becomes -90 X in rebedrock.
        bone.rotation = {-def.pose.rotation.x, -def.pose.rotation.y, def.pose.rotation.z};

        bone.cubes.reserve(def.cubes.cubes_.size());
        for (const CubeListBuilder::Entry& entry : def.cubes.cubes_) {
            ModelCube cube;
            cube.size = entry.size;
            cube.uv = entry.uv;
            cube.inflate = entry.inflate;
            cube.mirror = entry.mirror;

            if (entry.baked) {
                // Authored geo.json cube already in rebedrock space; copy through.
                cube.origin = entry.bakedOrigin;
                std::uint32_t packed = kIdentityFaceOverride;
                for (const FaceRelabel& relabel : entry.faces) {
                    packed = applyRelabel(packed, relabel);
                }
                cube.faceOverride = packed;
            } else {
                // Java model space -> rebedrock model space via scale(-1,-1,1):
                // mirror X and Y about the box's far corner (origin is the min
                // corner, so both axes use box_min + size), keep Z.
                const glm::vec3 boxMin = def.pose.offset + entry.box;
                cube.origin = {-(boxMin.x + entry.size.x),
                               kModelHeight - (boxMin.y + entry.size.y), boxMin.z};
                cube.faceOverride = kIdentityFaceOverride;
            }
            cube.pivot = cube.origin + cube.size * 0.5F;
            bone.cubes.push_back(std::move(cube));
        }

        bones.push_back(std::move(bone));
    }

    return SkeletalModel::assemble(std::move(identifier), textureWidth, textureHeight,
                                   std::move(bones));
}

} // namespace mc::animation
