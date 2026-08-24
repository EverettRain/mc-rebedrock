#include "animation/PartDefinition.hpp"
#include "animation/SkeletalModel.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

// RN-0a zero-drift regression: a model built through the C++ PartDefinition
// builder + baker must be *value-for-value identical* to the same model loaded
// from today's geo.json (SkeletalModel::parse). This is the guardrail that keeps
// the programmatic geometry path from silently changing any rendered species.
// boxUvFaceRect is untouched; the baker only reproduces the current convention.

namespace {

using mc::animation::CubeListBuilder;
using mc::animation::FaceRelabel;
using mc::animation::ModelBone;
using mc::animation::ModelCube;
using mc::animation::PartDefinition;
using mc::animation::PartPose;
using mc::animation::SkeletalModel;

bool near(float a, float b) { return std::abs(a - b) < 1e-4F; }

bool vecEq(const glm::vec3& a, const glm::vec3& b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}
bool vecEq(const glm::vec2& a, const glm::vec2& b) { return near(a.x, b.x) && near(a.y, b.y); }

void assertCubeEqual(const ModelCube& baked, const ModelCube& json) {
    assert(vecEq(baked.origin, json.origin));
    assert(vecEq(baked.size, json.size));
    assert(vecEq(baked.uv, json.uv));
    assert(vecEq(baked.pivot, json.pivot));
    assert(vecEq(baked.rotation, json.rotation));
    assert(near(baked.inflate, json.inflate));
    assert(baked.mirror == json.mirror);
    assert(baked.hasRotation == json.hasRotation);
    assert(baked.faceOverride == json.faceOverride);
}

void assertModelsEqual(const SkeletalModel& baked, const SkeletalModel& json) {
    assert(baked.identifier() == json.identifier());
    assert(baked.textureWidth() == json.textureWidth());
    assert(baked.textureHeight() == json.textureHeight());
    assert(baked.boneCount() == json.boneCount());
    for (std::size_t i = 0U; i < json.boneCount(); ++i) {
        const ModelBone& b = baked.bones()[i];
        const ModelBone& j = json.bones()[i];
        assert(b.name == j.name);
        assert(b.parent == j.parent);
        assert(vecEq(b.pivot, j.pivot));
        assert(vecEq(b.rotation, j.rotation));
        assert(b.neverRender == j.neverRender);
        assert(b.cubes.size() == j.cubes.size());
        for (std::size_t c = 0U; c < j.cubes.size(); ++c) {
            assertCubeEqual(b.cubes[c], j.cubes[c]);
        }
    }
}

// Today's resources/animation/cow.geo.json, embedded so the headless test is
// self-contained (mirrors box_uv_test's approach). This is the golden path the
// builder must reproduce byte-for-byte.
constexpr std::string_view kCowGeoJson = R"({
  "format_version": "1.12.0",
  "minecraft:geometry": [
    {
      "description": { "identifier": "geometry.cow", "texture_width": 64, "texture_height": 64 },
      "bones": [
        { "name": "body", "pivot": [0, 19, 2], "rotation": [90, 0, 0],
          "cubes": [
            { "origin": [-6, 9, -1], "size": [12, 18, 10], "uv": [18, 4],
              "faces": { "front": {"as": "back"}, "back": {"as": "front"} } },
            { "origin": [-2, 21, 9], "size": [4, 6, 1], "uv": [52, 0] }
          ] },
        { "name": "head", "pivot": [0, 20, -8],
          "cubes": [
            { "origin": [-4, 16, -14], "size": [8, 8, 6], "uv": [0, 0] },
            { "origin": [-3, 16, -15], "size": [6, 3, 1], "uv": [1, 33] },
            { "origin": [-5, 22, -13], "size": [1, 3, 1], "uv": [22, 0] },
            { "origin": [4, 22, -13], "size": [1, 3, 1], "uv": [22, 0] }
          ] },
        { "name": "legFrontRight", "pivot": [-4, 12, -5],
          "cubes": [ { "origin": [-6, 0, -7], "size": [4, 12, 4], "uv": [0, 16] } ] },
        { "name": "legFrontLeft", "pivot": [4, 12, -5],
          "cubes": [ { "origin": [2, 0, -7], "size": [4, 12, 4], "uv": [0, 16], "mirror": true } ] },
        { "name": "legBackRight", "pivot": [-4, 12, 7],
          "cubes": [ { "origin": [-6, 0, 5], "size": [4, 12, 4], "uv": [0, 16] } ] },
        { "name": "legBackLeft", "pivot": [4, 12, 7],
          "cubes": [ { "origin": [2, 0, 5], "size": [4, 12, 4], "uv": [0, 16], "mirror": true } ] }
      ]
    }
  ]
})";

// box-UV face indices used by the "faces" relabels below (0..5 = +X,-X,+Y,-Y,+Z,-Z).
constexpr int kBack = 4;
constexpr int kFront = 5;

// Transcribes today's cow.geo.json through the builder. head + four legs come
// straight from vanilla CowModel.createBodyLayer() (Java model space); the baker
// performs the Y-flip. The rotation-folded, face-compensated body is authored in
// rebedrock space via addBakedCube — that compensation is what RN-0b removes, so
// RN-0a reproduces it verbatim to guarantee zero visual drift.
SkeletalModel buildCow() {
    PartDefinition mesh;

    // Bone order must match the geo.json so index-by-index comparison holds.
    // body: cube 0 is the authored torso (faces relabel front<->back only; RN-0b
    // removed the up/down cap compensation now that boxUvFaceRect matches vanilla);
    // cube 1 the udder.
    CubeListBuilder body;
    body.addBakedCube(/*origin*/ {-6.0F, 9.0F, -1.0F}, /*size*/ {12.0F, 18.0F, 10.0F},
                      /*uv*/ {18.0F, 4.0F}, /*texU*/ 18, /*texV*/ 4, /*mirror*/ false,
                      /*inflate*/ 0.0F,
                      // geo.json "<key>": {"as": "<value>"} => target = index(value),
                      // pos = index(key). front->back, back->front.
                      {FaceRelabel{/*target=back*/ kBack, /*pos=front*/ kFront, false},
                       FaceRelabel{/*target=front*/ kFront, /*pos=back*/ kBack, false}});
    body.addBakedCube({-2.0F, 21.0F, 9.0F}, {4.0F, 6.0F, 1.0F}, {52.0F, 0.0F}, 52, 0);
    mesh.addOrReplaceChild("body", body, PartPose::offsetAndRotation(0.0F, 5.0F, 2.0F, 90.0F, 0.0F, 0.0F));

    // head: vanilla addBox values (Java Y-down), baker flips Y.
    CubeListBuilder head;
    head.texOffs(0, 0).addBox(-4.0F, -4.0F, -6.0F, 8.0F, 8.0F, 6.0F);
    head.texOffs(1, 33).addBox(-3.0F, 1.0F, -7.0F, 6.0F, 3.0F, 1.0F);
    head.texOffs(22, 0).addBox(-5.0F, -5.0F, -5.0F, 1.0F, 3.0F, 1.0F);  // right horn
    head.texOffs(22, 0).addBox(4.0F, -5.0F, -5.0F, 1.0F, 3.0F, 1.0F);   // left horn
    mesh.addOrReplaceChild("head", head, PartPose::offsetPose(0.0F, 4.0F, -8.0F));

    // legs: vanilla addBox; right un-mirrored, left mirrored.
    CubeListBuilder rightFront;
    rightFront.texOffs(0, 16).addBox(-2.0F, 0.0F, -2.0F, 4.0F, 12.0F, 4.0F);
    mesh.addOrReplaceChild("legFrontRight", rightFront, PartPose::offsetPose(-4.0F, 12.0F, -5.0F));

    CubeListBuilder leftFront;
    leftFront.mirror().texOffs(0, 16).addBox(-2.0F, 0.0F, -2.0F, 4.0F, 12.0F, 4.0F);
    mesh.addOrReplaceChild("legFrontLeft", leftFront, PartPose::offsetPose(4.0F, 12.0F, -5.0F));

    CubeListBuilder rightBack;
    rightBack.texOffs(0, 16).addBox(-2.0F, 0.0F, -2.0F, 4.0F, 12.0F, 4.0F);
    mesh.addOrReplaceChild("legBackRight", rightBack, PartPose::offsetPose(-4.0F, 12.0F, 7.0F));

    CubeListBuilder leftBack;
    leftBack.mirror().texOffs(0, 16).addBox(-2.0F, 0.0F, -2.0F, 4.0F, 12.0F, 4.0F);
    mesh.addOrReplaceChild("legBackLeft", leftBack, PartPose::offsetPose(4.0F, 12.0F, 7.0F));

    return mesh.bake("geometry.cow", 64, 64);
}

// A clean, fully-vanilla-transcribed model (no authored escape hatch): the
// quadruped-sized head + four legs alone, proving the Java->rebedrock transform
// is exact end to end for every non-rotated part. Compared to the head/leg
// bones of the golden cow.
constexpr std::string_view kHeadLegsGeoJson = R"({
  "format_version": "1.12.0",
  "minecraft:geometry": [
    { "description": { "identifier": "geometry.headlegs", "texture_width": 64, "texture_height": 64 },
      "bones": [
        { "name": "head", "pivot": [0, 20, -8],
          "cubes": [ { "origin": [-4, 16, -14], "size": [8, 8, 6], "uv": [0, 0] } ] },
        { "name": "legFrontRight", "pivot": [-4, 12, -5],
          "cubes": [ { "origin": [-6, 0, -7], "size": [4, 12, 4], "uv": [0, 16] } ] }
      ]
    }
  ]
})";

SkeletalModel buildHeadLegs() {
    PartDefinition mesh;
    CubeListBuilder head;
    head.texOffs(0, 0).addBox(-4.0F, -4.0F, -6.0F, 8.0F, 8.0F, 6.0F);
    mesh.addOrReplaceChild("head", head, PartPose::offsetPose(0.0F, 4.0F, -8.0F));
    CubeListBuilder leg;
    leg.texOffs(0, 16).addBox(-2.0F, 0.0F, -2.0F, 4.0F, 12.0F, 4.0F);
    mesh.addOrReplaceChild("legFrontRight", leg, PartPose::offsetPose(-4.0F, 12.0F, -5.0F));
    return mesh.bake("geometry.headlegs", 64, 64);
}

} // namespace

int main() {
    // Golden: full cow, builder vs geo.json, value for value.
    const SkeletalModel jsonCow = SkeletalModel::parse(kCowGeoJson, "geometry.cow");
    const SkeletalModel bakedCow = buildCow();
    assertModelsEqual(bakedCow, jsonCow);

    // Pure vanilla transform (no addBakedCube) still reproduces geo.json exactly.
    const SkeletalModel jsonHL = SkeletalModel::parse(kHeadLegsGeoJson, "geometry.headlegs");
    const SkeletalModel bakedHL = buildHeadLegs();
    assertModelsEqual(bakedHL, jsonHL);

    // Parent resolution: a child may name a parent declared after it, matching
    // loadGeometry's two passes.
    {
        PartDefinition mesh;
        CubeListBuilder empty;
        mesh.addOrReplaceChild("child", empty, PartPose::offsetPose(0.0F, 0.0F, 0.0F), "root");
        mesh.addOrReplaceChild("root", empty, PartPose::offsetPose(0.0F, 0.0F, 0.0F));
        const SkeletalModel m = mesh.bake("geometry.parented", 16, 16);
        assert(m.findBone("child") == 0);
        assert(m.findBone("root") == 1);
        assert(m.bones()[0].parent == 1);  // child -> root (index 1)
        assert(m.bones()[1].parent == -1); // root has no parent
    }

    // inflate carries through as-is; renderSize/center match geo.json semantics.
    {
        PartDefinition mesh;
        CubeListBuilder c;
        c.texOffs(0, 0).addBox(-2.0F, 0.0F, -2.0F, 4.0F, 4.0F, 4.0F, mc::animation::CubeDeformation{0.5F});
        mesh.addOrReplaceChild("b", c, PartPose::offsetPose(0.0F, 0.0F, 0.0F));
        const SkeletalModel infModel = mesh.bake("geometry.inf", 16, 16);
        const ModelCube& cube = infModel.bones()[0].cubes[0];
        assert(near(cube.inflate, 0.5F));
        assert(vecEq(cube.renderSize(), glm::vec3{5.0F, 5.0F, 5.0F}));
    }

    // bake-not-parse: bake() must return a self-contained snapshot, never a live
    // interpretation of the definition. Mutating (and even destroying) the source
    // PartDefinition after baking must leave the baked model untouched. A baker
    // that interpreted the definition per use would drift here.
    {
        SkeletalModel snapshot;
        {
            PartDefinition mesh;
            CubeListBuilder c;
            c.texOffs(0, 0).addBox(-2.0F, 0.0F, -2.0F, 4.0F, 4.0F, 4.0F);
            mesh.addOrReplaceChild("b", c, PartPose::offsetPose(0.0F, 0.0F, 0.0F));
            snapshot = mesh.bake("geometry.snap", 16, 16);
            // Mutate the source after baking; then the builder falls out of scope.
            mesh.addOrReplaceChild("b", CubeListBuilder::create().texOffs(9, 9).addBox(
                                            10.0F, 10.0F, 10.0F, 2.0F, 2.0F, 2.0F),
                                   PartPose::offsetPose(5.0F, 5.0F, 5.0F));
        }
        assert(snapshot.boneCount() == 1);
        assert(snapshot.bones()[0].cubes.size() == 1);
        assert(vecEq(snapshot.bones()[0].cubes[0].size, glm::vec3{4.0F, 4.0F, 4.0F}));
        assert(vecEq(snapshot.bones()[0].cubes[0].uv, glm::vec2{0.0F, 0.0F}));
        assert(vecEq(snapshot.bones()[0].pivot,
                     glm::vec3{0.0F, mc::animation::kModelHeight, 0.0F}));
    }

    return 0;
}
