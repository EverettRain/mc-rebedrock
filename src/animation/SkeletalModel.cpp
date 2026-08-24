#include "animation/SkeletalModel.hpp"

#include "core/Json.hpp"

#include <glm/ext/matrix_transform.hpp>

#include <stdexcept>

namespace mc::animation {

glm::mat4 rotationMatrix(const glm::vec3& degrees) {
    constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
    glm::mat4 matrix{1.0F};
    matrix = glm::rotate(matrix, degrees.z * kDegToRad, {0.0F, 0.0F, 1.0F});
    matrix = glm::rotate(matrix, degrees.y * kDegToRad, {0.0F, 1.0F, 0.0F});
    matrix = glm::rotate(matrix, degrees.x * kDegToRad, {1.0F, 0.0F, 0.0F});
    return matrix;
}

glm::mat4 rotationAboutPivot(const glm::vec3& degrees, const glm::vec3& pivot) {
    return glm::translate(glm::mat4{1.0F}, pivot) * rotationMatrix(degrees) *
           glm::translate(glm::mat4{1.0F}, -pivot);
}

BoxUvRect boxUvFaceRect(int face, glm::vec2 uv, glm::vec3 size) {
    // Minecraft box-UV net (u right, v down). `face` is the GEOMETRIC cube face
    // (0..5 = +X,-X,+Y,-Y,+Z,-Z). The geometry baker applies the full vanilla
    // scale(-1,-1,1), so a rebedrock cube equals `Java ModelPart.Cube ∘ scale(-1,
    // -1,1)`: geometric +X == vanilla WEST polygon, -X == EAST, +Y == DOWN (left
    // cap), -Y == UP (right cap), -Z == NORTH (front), +Z == SOUTH (back). We
    // therefore reproduce ModelPart's rects mapped through that correspondence:
    //
    //           +----+----+
    //           | +Y | -Y |            top-row caps: +Y left (u1..u2), -Y right (u2..u22)
    //      +----+----+----+----+
    //      | +X | -Z | -X | +Z |       middle row = vanilla WEST,NORTH,EAST,SOUTH
    //      +----+----+----+----+       i.e. mob's Right, Front, Left, Back
    //
    // The X mapping here (+X -> leftmost/"west" rect, -X -> third/"east" rect) is
    // NOT a naming typo: after the baker's X flip, geometric +X is physically the
    // mob's right, which vanilla paints from its WEST (minX) polygon. See docs
    // RN-0c-x-flip-root-cause.md. Likewise +Y (physical top) samples the LEFT cap
    // because vanilla's Y-down DOWN polygon (minY) is physically the top under the
    // scale flip (do not "fix" +Y/-Y back to the enum names; that was 146f7bd).
    //
    // This matches the reference in tools/entity_uv_lib.py (which the texture editor
    // mirrors) and the box-UV vertex shader. The rect is built from the cube's
    // declared size; `inflate` grows the drawn box but never the net.
    const float sx = size.x;
    const float sy = size.y;
    const float sz = size.z;
    switch (face) {
    case 0: // +X (mob right) -> vanilla WEST rect (leftmost middle)
        return {{uv.x, uv.y + sz}, {sz, sy}};
    case 1: // -X (mob left) -> vanilla EAST rect (third middle)
        return {{uv.x + sz + sx, uv.y + sz}, {sz, sy}};
    case 2: // +Y (up) -> left cap
        return {{uv.x + sz, uv.y}, {sx, sz}};
    case 3: // -Y (down) -> right cap
        return {{uv.x + sz + sx, uv.y}, {sx, sz}};
    case 4: // +Z (back)
        return {{uv.x + 2.0F * sz + sx, uv.y + sz}, {sx, sy}};
    default: // 5: -Z (front)
        return {{uv.x + sz, uv.y + sz}, {sx, sy}};
    }
}

namespace {

[[nodiscard]] glm::vec3 readVec3(const core::Json& value, const glm::vec3& fallback) {
    if (!value.isArray() || value.size() < 3U) {
        return fallback;
    }
    return {value[0].asFloat(fallback.x), value[1].asFloat(fallback.y),
            value[2].asFloat(fallback.z)};
}

[[nodiscard]] glm::vec2 readVec2(const core::Json& value, const glm::vec2& fallback) {
    if (!value.isArray() || value.size() < 2U) {
        return fallback;
    }
    return {value[0].asFloat(fallback.x), value[1].asFloat(fallback.y)};
}

// box-UV face name -> index (0..5 = +X east, -X west, +Y up, -Y down, +Z back,
// -Z front), matching boxUvFaceRect and the entity shader.
[[nodiscard]] int boxUvFaceIndex(std::string_view name) {
    if (name == "east") return 0;
    if (name == "west") return 1;
    if (name == "up") return 2;
    if (name == "down") return 3;
    if (name == "back") return 4;
    if (name == "front") return 5;
    return -1;
}

} // namespace

int SkeletalModel::findBone(std::string_view name) const {
    const auto it = boneIndex_.find(std::string{name});
    return it != boneIndex_.end() ? it->second : -1;
}

SkeletalModel SkeletalModel::loadGeometry(const core::Json& document, std::string_view identifier) {
    const core::Json& geometries = document["minecraft:geometry"];
    if (!geometries.isArray() || geometries.size() == 0U) {
        throw std::runtime_error("geometry document is missing a 'minecraft:geometry' array");
    }

    const core::Json* selected = nullptr;
    if (identifier.empty()) {
        selected = &geometries[0];
    } else {
        for (std::size_t i = 0U; i < geometries.size(); ++i) {
            if (geometries[i]["description"]["identifier"].asString() == identifier) {
                selected = &geometries[i];
                break;
            }
        }
        if (selected == nullptr) {
            throw std::runtime_error("geometry '" + std::string{identifier} + "' not found");
        }
    }

    const core::Json& description = (*selected)["description"];
    SkeletalModel model;
    model.identifier_ = description["identifier"].asString();
    model.textureWidth_ = static_cast<int>(description["texture_width"].asNumber(16.0));
    model.textureHeight_ = static_cast<int>(description["texture_height"].asNumber(16.0));

    const core::Json& bones = (*selected)["bones"];
    if (!bones.isArray()) {
        throw std::runtime_error("geometry '" + model.identifier_ + "' has no bones array");
    }

    // First pass: create bones and register names so parents resolve regardless
    // of declaration order.
    model.bones_.reserve(bones.size());
    for (std::size_t i = 0U; i < bones.size(); ++i) {
        const core::Json& boneJson = bones[i];
        ModelBone bone;
        bone.name = boneJson["name"].asString();
        if (bone.name.empty()) {
            throw std::runtime_error("geometry '" + model.identifier_ + "' has an unnamed bone");
        }
        bone.pivot = readVec3(boneJson["pivot"], glm::vec3{0.0F});
        bone.rotation = readVec3(boneJson["rotation"], glm::vec3{0.0F});
        bone.neverRender = boneJson["neverRender"].asBool(false);

        const core::Json& cubes = boneJson["cubes"];
        if (cubes.isArray()) {
            bone.cubes.reserve(cubes.size());
            for (std::size_t c = 0U; c < cubes.size(); ++c) {
                const core::Json& cubeJson = cubes[c];
                ModelCube cube;
                cube.origin = readVec3(cubeJson["origin"], glm::vec3{0.0F});
                cube.size = readVec3(cubeJson["size"], glm::vec3{0.0F});
                cube.uv = readVec2(cubeJson["uv"], glm::vec2{0.0F});
                cube.inflate = cubeJson["inflate"].asFloat(0.0F);
                cube.mirror = cubeJson["mirror"].asBool(false);
                if (cubeJson.contains("rotation")) {
                    cube.rotation = readVec3(cubeJson["rotation"], glm::vec3{0.0F});
                    cube.hasRotation = true;
                }
                cube.pivot = readVec3(cubeJson["pivot"], cube.origin + cube.size * 0.5F);

                // The texture editor's "faces" extension re-routes this cube's
                // box-UV rects: each entry `"<netPos>": {"as": "<face>", "rotate":
                // 0|180}` hands the rect at netPos to the geometric face <face>,
                // optionally sampling it rotated 180°. Unknown names are ignored.
                const core::Json& faceOverrides = cubeJson["faces"];
                if (faceOverrides.isObject()) {
                    for (const auto& [posName, entry] : faceOverrides.asObject()) {
                        const int pos = boxUvFaceIndex(posName);
                        if (pos < 0) {
                            continue;
                        }
                        std::string as = posName;
                        int rotate = 0;
                        if (entry.isString()) {
                            as = entry.asString();
                        } else if (entry.isObject()) {
                            const core::Json& asJson = entry["as"];
                            if (asJson.isString() && !asJson.asString().empty()) {
                                as = asJson.asString();
                            }
                            rotate = static_cast<int>(entry["rotate"].asNumber(0.0));
                        }
                        const int target = boxUvFaceIndex(as);
                        if (target < 0) {
                            continue;
                        }
                        const std::uint32_t mask = 0xFU << (target * 4U);
                        cube.faceOverride &= ~mask;
                        cube.faceOverride |= static_cast<std::uint32_t>(pos) << (target * 4U);
                        if (rotate == 180) {
                            cube.faceOverride |= (1U << (target * 4U + 3U));
                        }
                    }
                }

                bone.cubes.push_back(cube);
            }
        }

        const int index = static_cast<int>(model.bones_.size());
        if (!model.boneIndex_.emplace(bone.name, index).second) {
            throw std::runtime_error("geometry '" + model.identifier_ +
                                     "' has a duplicate bone '" + bone.name + "'");
        }
        model.bones_.push_back(std::move(bone));
    }

    // Second pass: resolve parent references by name.
    for (std::size_t i = 0U; i < bones.size(); ++i) {
        const core::Json& parent = bones[i]["parent"];
        if (parent.isString() && !parent.asString().empty()) {
            const int parentIndex = model.findBone(parent.asString());
            if (parentIndex < 0) {
                throw std::runtime_error("bone '" + model.bones_[i].name + "' references unknown "
                                         "parent '" + parent.asString() + "'");
            }
            model.bones_[i].parent = parentIndex;
        }
    }

    return model;
}

SkeletalModel SkeletalModel::parse(std::string_view jsonText, std::string_view identifier) {
    return loadGeometry(core::Json::parse(jsonText), identifier);
}

SkeletalModel SkeletalModel::assemble(std::string identifier, int textureWidth, int textureHeight,
                                      std::vector<ModelBone> bones) {
    SkeletalModel model;
    model.identifier_ = std::move(identifier);
    model.textureWidth_ = textureWidth;
    model.textureHeight_ = textureHeight;
    model.bones_ = std::move(bones);

    // Register names first so parents resolve regardless of declaration order,
    // matching loadGeometry's two-pass behaviour. Bones already carry a resolved
    // `parent` index (-1 for roots) from the baker; assemble only validates.
    model.boneIndex_.reserve(model.bones_.size());
    for (std::size_t i = 0U; i < model.bones_.size(); ++i) {
        const ModelBone& bone = model.bones_[i];
        if (bone.name.empty()) {
            throw std::runtime_error("assembled geometry '" + model.identifier_ +
                                     "' has an unnamed bone");
        }
        if (!model.boneIndex_.emplace(bone.name, static_cast<int>(i)).second) {
            throw std::runtime_error("assembled geometry '" + model.identifier_ +
                                     "' has a duplicate bone '" + bone.name + "'");
        }
    }
    for (const ModelBone& bone : model.bones_) {
        if (bone.parent >= static_cast<int>(model.bones_.size())) {
            throw std::runtime_error("assembled geometry '" + model.identifier_ +
                                     "' bone '" + bone.name + "' has an out-of-range parent");
        }
    }
    return model;
}

} // namespace mc::animation
