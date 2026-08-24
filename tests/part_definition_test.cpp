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
        { "name": "body", "pivot": [0, 19, 2], "rotation": [-90, 0, 0],
          "cubes": [
            { "origin": [-6, 9, -1], "size": [12, 18, 10], "uv": [18, 4] },
            { "origin": [-2, 21, 9], "size": [4, 6, 1], "uv": [52, 0] }
          ] },
        { "name": "head", "pivot": [0, 20, -8],
          "cubes": [
            { "origin": [-4, 16, -14], "size": [8, 8, 6], "uv": [0, 0] },
            { "origin": [-3, 16, -15], "size": [6, 3, 1], "uv": [1, 33] },
            { "origin": [4, 22, -13], "size": [1, 3, 1], "uv": [22, 0] },
            { "origin": [-5, 22, -13], "size": [1, 3, 1], "uv": [22, 0] }
          ] },
        { "name": "legFrontRight", "pivot": [4, 12, -5],
          "cubes": [ { "origin": [2, 0, -7], "size": [4, 12, 4], "uv": [0, 16] } ] },
        { "name": "legFrontLeft", "pivot": [-4, 12, -5],
          "cubes": [ { "origin": [-6, 0, -7], "size": [4, 12, 4], "uv": [0, 16], "mirror": true } ] },
        { "name": "legBackRight", "pivot": [4, 12, 7],
          "cubes": [ { "origin": [2, 0, 5], "size": [4, 12, 4], "uv": [0, 16] } ] },
        { "name": "legBackLeft", "pivot": [-4, 12, 7],
          "cubes": [ { "origin": [-6, 0, 5], "size": [4, 12, 4], "uv": [0, 16], "mirror": true } ] }
      ]
    }
  ]
})";

// Transcribes today's cow.geo.json through the builder. head + four legs come
// straight from vanilla CowModel.createBodyLayer() (Java model space); the baker
// performs the full scale(-1,-1,1). The rotated torso is authored in rebedrock
// space via addBakedCube; under RN-0c it carries no faces compensation.
SkeletalModel buildCow() {
    PartDefinition mesh;

    // Bone order must match the geo.json so index-by-index comparison holds.
    // Under RN-0c the baker applies the full scale(-1,-1,1), so the rotated torso
    // needs no per-face compensation: authored torso (no faces override) + udder.
    // Origins are symmetric in X, so the flip leaves these baked values unchanged.
    CubeListBuilder body;
    body.addBakedCube(/*origin*/ {-6.0F, 9.0F, -1.0F}, /*size*/ {12.0F, 18.0F, 10.0F},
                      /*uv*/ {18.0F, 4.0F}, /*texU*/ 18, /*texV*/ 4, /*mirror*/ false,
                      /*inflate*/ 0.0F, /*faces*/ {});
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
        { "name": "legFrontRight", "pivot": [4, 12, -5],
          "cubes": [ { "origin": [2, 0, -7], "size": [4, 12, 4], "uv": [0, 16] } ] }
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

// --- RN-1: species transcription guards ------------------------------------
//
// RN-1 aligns the shipped geo.json for cow/chicken/sheep/zombie to JE 26.1's
// createBodyLayer(). These guards transcribe the relevant 26.1 mesh through the
// same builder+baker and pin the load-bearing values RN-1 changed, so a
// regression in the disk geo.json (or a mis-transcription) is caught headless.
// The baker's Java->rebedrock transform is already proven exact above; here we
// assert the *values* RN-1 depends on, plus the double-layer / hat structure.

[[nodiscard]] const ModelCube* firstCube(const SkeletalModel& m, std::string_view bone) {
    const int idx = m.findBone(bone);
    if (idx < 0 || m.bones()[static_cast<std::size_t>(idx)].cubes.empty()) {
        return nullptr;
    }
    return &m.bones()[static_cast<std::size_t>(idx)].cubes.front();
}

// JE 26.1 SheepModel (base layer): QuadrupedModel.createBodyMesh(12, false, true)
// with head/body overridden. legSize=12 so legs are 4x12x4; the RIGHT legs mirror
// (mirrorRightLeg=true), the LEFT do not.
SkeletalModel buildSheepBase() {
    PartDefinition mesh;
    CubeListBuilder head;
    head.texOffs(0, 0).addBox(-3.0F, -4.0F, -6.0F, 6.0F, 6.0F, 8.0F);
    mesh.addOrReplaceChild("head", head, PartPose::offsetPose(0.0F, 6.0F, -8.0F));

    // body is rotated +90 X; authored in rebedrock space via addBakedCube. Under
    // RN-0c (full scale(-1,-1,1)) it needs no faces compensation. X-symmetric, so
    // the flip leaves the baked origin unchanged.
    CubeListBuilder body;
    body.addBakedCube({-4.0F, 9.0F, 3.0F}, {8.0F, 16.0F, 6.0F}, {28.0F, 8.0F}, 28, 8, false, 0.0F);
    mesh.addOrReplaceChild("body", body, PartPose::offsetAndRotation(0.0F, 5.0F, 2.0F, 90.0F, 0.0F, 0.0F));

    const auto leg = [](bool mirror) {
        CubeListBuilder c;
        c.mirror(mirror).texOffs(0, 16).addBox(-2.0F, 0.0F, -2.0F, 4.0F, 12.0F, 4.0F);
        return c;
    };
    mesh.addOrReplaceChild("legFrontRight", leg(true), PartPose::offsetPose(-3.0F, 12.0F, -5.0F));
    mesh.addOrReplaceChild("legFrontLeft", leg(false), PartPose::offsetPose(3.0F, 12.0F, -5.0F));
    mesh.addOrReplaceChild("legBackRight", leg(true), PartPose::offsetPose(-3.0F, 12.0F, 7.0F));
    mesh.addOrReplaceChild("legBackLeft", leg(false), PartPose::offsetPose(3.0F, 12.0F, 7.0F));
    return mesh.bake("geometry.sheep", 64, 32);
}

// JE 26.1 SheepFurModel: head inflate 0.6, body inflate 1.75 (rotated), legs
// 4x6x4 inflate 0.5. In the shipped single-model geo.json the fur bones are
// children of their base counterparts; here we only pin the cube values.
SkeletalModel buildSheepFur() {
    PartDefinition mesh;
    CubeListBuilder head;
    head.texOffs(0, 0).addBox(-3.0F, -4.0F, -4.0F, 6.0F, 6.0F, 6.0F, mc::animation::CubeDeformation{0.6F});
    mesh.addOrReplaceChild("woolHead", head, PartPose::offsetPose(0.0F, 6.0F, -8.0F));

    CubeListBuilder body;
    body.addBakedCube({-4.0F, 9.0F, 3.0F}, {8.0F, 16.0F, 6.0F}, {28.0F, 8.0F}, 28, 8, false, 1.75F);
    mesh.addOrReplaceChild("wool", body,
                           PartPose::offsetAndRotation(0.0F, 5.0F, 2.0F, 90.0F, 0.0F, 0.0F));

    const auto furLeg = []() {
        CubeListBuilder c;
        c.texOffs(0, 16).addBox(-2.0F, 0.0F, -2.0F, 4.0F, 6.0F, 4.0F, mc::animation::CubeDeformation{0.5F});
        return c;
    };
    mesh.addOrReplaceChild("woolLegFrontRight", furLeg(), PartPose::offsetPose(-3.0F, 12.0F, -5.0F));
    mesh.addOrReplaceChild("woolLegBackRight", furLeg(), PartPose::offsetPose(-3.0F, 12.0F, 7.0F));
    return mesh.bake("geometry.sheepfur", 64, 32);
}

// JE 26.1 AdultChickenModel.createBaseChickenModel(): head + beak + redThing +
// rotated body + two legs (uv 26,0, no mirror) + two wings (uv 24,13, no mirror).
SkeletalModel buildChicken() {
    PartDefinition mesh;
    CubeListBuilder head;
    head.texOffs(0, 0).addBox(-2.0F, -6.0F, -2.0F, 4.0F, 6.0F, 3.0F);
    mesh.addOrReplaceChild("head", head, PartPose::offsetPose(0.0F, 15.0F, -4.0F));

    CubeListBuilder beak;
    beak.texOffs(14, 0).addBox(-2.0F, -4.0F, -4.0F, 4.0F, 2.0F, 2.0F);
    mesh.addOrReplaceChild("beak", beak, PartPose::offsetPose(0.0F, 15.0F, -4.0F), "head");

    CubeListBuilder red;
    red.texOffs(14, 4).addBox(-1.0F, -2.0F, -3.0F, 2.0F, 2.0F, 2.0F);
    mesh.addOrReplaceChild("redThing", red, PartPose::offsetPose(0.0F, 15.0F, -4.0F), "head");

    CubeListBuilder body;
    body.addBakedCube({-3.0F, 4.0F, -3.0F}, {6.0F, 8.0F, 6.0F}, {0.0F, 9.0F}, 0, 9, false, 0.0F);
    mesh.addOrReplaceChild("body", body, PartPose::offsetAndRotation(0.0F, 16.0F, 0.0F, 90.0F, 0.0F, 0.0F));

    CubeListBuilder rl;
    rl.texOffs(26, 0).addBox(-1.0F, 0.0F, -3.0F, 3.0F, 5.0F, 3.0F);
    mesh.addOrReplaceChild("rightLeg", rl, PartPose::offsetPose(-2.0F, 19.0F, 1.0F));
    CubeListBuilder ll;
    ll.texOffs(26, 0).addBox(-1.0F, 0.0F, -3.0F, 3.0F, 5.0F, 3.0F);
    mesh.addOrReplaceChild("leftLeg", ll, PartPose::offsetPose(1.0F, 19.0F, 1.0F));

    CubeListBuilder rw;
    rw.texOffs(24, 13).addBox(0.0F, 0.0F, -3.0F, 1.0F, 4.0F, 6.0F);
    mesh.addOrReplaceChild("rightWing", rw, PartPose::offsetPose(-4.0F, 13.0F, 0.0F));
    CubeListBuilder lw;
    lw.texOffs(24, 13).addBox(-1.0F, 0.0F, -3.0F, 1.0F, 4.0F, 6.0F);
    mesh.addOrReplaceChild("leftWing", lw, PartPose::offsetPose(4.0F, 13.0F, 0.0F));
    return mesh.bake("geometry.chicken", 64, 32);
}

// JE 26.1 HumanoidModel.createMesh(NONE, 0): the zombie body/head/arms/legs plus
// the head-child hat overlay (inflate 0.5, texOffs 32,0).
SkeletalModel buildZombieHead() {
    PartDefinition mesh;
    CubeListBuilder head;
    head.texOffs(0, 0).addBox(-4.0F, -8.0F, -4.0F, 8.0F, 8.0F, 8.0F);
    mesh.addOrReplaceChild("head", head, PartPose::offsetPose(0.0F, 0.0F, 0.0F));
    CubeListBuilder hat;
    hat.texOffs(32, 0).addBox(-4.0F, -8.0F, -4.0F, 8.0F, 8.0F, 8.0F, mc::animation::CubeDeformation{0.5F});
    mesh.addOrReplaceChild("hat", hat, PartPose::offsetPose(0.0F, 0.0F, 0.0F), "head");
    return mesh.bake("geometry.zombie", 64, 64);
}

// The shipped resources/animation/sheep.geo.json (RN-1 double-layer rebuild),
// embedded so the headless test stays self-contained.
constexpr std::string_view kSheepGeoJson = R"({
  "format_version": "1.12.0",
  "minecraft:geometry": [
    { "description": { "identifier": "geometry.sheep", "texture_width": 64, "texture_height": 32 },
      "bones": [
        { "name": "head", "pivot": [0,18,-8], "cubes": [ { "origin": [-3,16,-14], "size": [6,6,8], "uv": [0,0] } ] },
        { "name": "body", "pivot": [0,19,2], "rotation": [-90,0,0],
          "cubes": [ { "origin": [-4,9,3], "size": [8,16,6], "uv": [28,8] } ] },
        { "name": "legFrontRight", "pivot": [3,12,-5], "cubes": [ { "origin": [1,0,-7], "size": [4,12,4], "uv": [0,16], "mirror": true } ] },
        { "name": "legFrontLeft", "pivot": [-3,12,-5], "cubes": [ { "origin": [-5,0,-7], "size": [4,12,4], "uv": [0,16] } ] },
        { "name": "legBackRight", "pivot": [3,12,7], "cubes": [ { "origin": [1,0,5], "size": [4,12,4], "uv": [0,16], "mirror": true } ] },
        { "name": "legBackLeft", "pivot": [-3,12,7], "cubes": [ { "origin": [-5,0,5], "size": [4,12,4], "uv": [0,16] } ] },
        { "name": "woolHead", "parent": "head", "pivot": [0,18,-8], "cubes": [ { "origin": [-3,16,-12], "size": [6,6,6], "uv": [0,0], "inflate": 0.6 } ] },
        { "name": "wool", "parent": "body", "pivot": [0,19,2], "rotation": [-90,0,0],
          "cubes": [ { "origin": [-4,9,3], "size": [8,16,6], "uv": [28,8], "inflate": 1.75 } ] },
        { "name": "woolLegFrontRight", "parent": "legFrontRight", "pivot": [3,12,-5], "cubes": [ { "origin": [1,6,-7], "size": [4,6,4], "uv": [0,16], "inflate": 0.5 } ] },
        { "name": "woolLegFrontLeft", "parent": "legFrontLeft", "pivot": [-3,12,-5], "cubes": [ { "origin": [-5,6,-7], "size": [4,6,4], "uv": [0,16], "inflate": 0.5 } ] },
        { "name": "woolLegBackRight", "parent": "legBackRight", "pivot": [3,12,7], "cubes": [ { "origin": [1,6,5], "size": [4,6,4], "uv": [0,16], "inflate": 0.5 } ] },
        { "name": "woolLegBackLeft", "parent": "legBackLeft", "pivot": [-3,12,7], "cubes": [ { "origin": [-5,6,5], "size": [4,6,4], "uv": [0,16], "inflate": 0.5 } ] }
      ] } ] })";

void assertSheepDisk() {
    const SkeletalModel disk = SkeletalModel::parse(kSheepGeoJson, "geometry.sheep");
    const SkeletalModel base = buildSheepBase();
    const SkeletalModel fur = buildSheepFur();

    // Base layer: legs are 4x12x4 (legSize=12, NOT the old 1.16.1 legSize=10).
    for (const char* leg : {"legFrontRight", "legFrontLeft", "legBackRight", "legBackLeft"}) {
        const ModelCube* d = firstCube(disk, leg);
        const ModelCube* b = firstCube(base, leg);
        assert(d != nullptr && b != nullptr);
        assert(vecEq(d->size, glm::vec3{4.0F, 12.0F, 4.0F}));
        assertCubeEqual(*b, *d);
    }
    // Base right legs mirror, left legs do not (26.1 SheepModel mirrorRightLeg=true).
    assert(firstCube(disk, "legFrontRight")->mirror);
    assert(firstCube(disk, "legBackRight")->mirror);
    assert(!firstCube(disk, "legFrontLeft")->mirror);
    assert(!firstCube(disk, "legBackLeft")->mirror);
    // Base body/head match the transcription (head is 6x6x8, not the old二盒 form).
    assertCubeEqual(*firstCube(disk, "head"), *firstCube(base, "head"));
    assert(vecEq(firstCube(disk, "head")->size, glm::vec3{6.0F, 6.0F, 8.0F}));
    assertCubeEqual(*firstCube(disk, "body"), *firstCube(base, "body"));

    // Fur layer exists as a second set of bones with the 26.1 inflate values.
    assert(disk.findBone("woolHead") >= 0);
    assert(disk.findBone("wool") >= 0);
    assert(near(firstCube(disk, "woolHead")->inflate, 0.6F));
    assert(near(firstCube(disk, "wool")->inflate, 1.75F));
    assertCubeEqual(*firstCube(disk, "woolHead"), *firstCube(fur, "woolHead"));
    assertCubeEqual(*firstCube(disk, "wool"), *firstCube(fur, "wool"));
    for (const char* leg :
         {"woolLegFrontRight", "woolLegFrontLeft", "woolLegBackRight", "woolLegBackLeft"}) {
        const ModelCube* d = firstCube(disk, leg);
        assert(d != nullptr);
        assert(vecEq(d->size, glm::vec3{4.0F, 6.0F, 4.0F})); // legSize=6 fur legs
        assert(near(d->inflate, 0.5F));
        assert(!d->mirror); // fur legs never mirror (single reused builder)
    }
    // Fur legs inherit their base leg's animation via a bone parent.
    assert(disk.bones()[static_cast<std::size_t>(disk.findBone("woolLegFrontRight"))].parent ==
           disk.findBone("legFrontRight"));
}

// The shipped resources/animation/chicken.geo.json (RN-1 26.1 transcription).
constexpr std::string_view kChickenGeoJson = R"({
  "format_version": "1.12.0",
  "minecraft:geometry": [
    { "description": { "identifier": "geometry.chicken", "texture_width": 64, "texture_height": 32 },
      "bones": [
        { "name": "head", "pivot": [0,9,-4], "cubes": [ { "origin": [-2,9,-6], "size": [4,6,3], "uv": [0,0] } ] },
        { "name": "beak", "parent": "head", "pivot": [0,9,-4], "cubes": [ { "origin": [-2,11,-8], "size": [4,2,2], "uv": [14,0] } ] },
        { "name": "redThing", "parent": "head", "pivot": [0,9,-4], "cubes": [ { "origin": [-1,9,-7], "size": [2,2,2], "uv": [14,4] } ] },
        { "name": "body", "pivot": [0,8,0], "rotation": [-90,0,0],
          "cubes": [ { "origin": [-3,4,-3], "size": [6,8,6], "uv": [0,9] } ] },
        { "name": "rightLeg", "pivot": [2,5,1], "cubes": [ { "origin": [0,0,-2], "size": [3,5,3], "uv": [26,0] } ] },
        { "name": "leftLeg", "pivot": [-1,5,1], "cubes": [ { "origin": [-3,0,-2], "size": [3,5,3], "uv": [26,0] } ] },
        { "name": "rightWing", "pivot": [4,11,0], "cubes": [ { "origin": [3,7,-3], "size": [1,4,6], "uv": [24,13] } ] },
        { "name": "leftWing", "pivot": [-4,11,0], "cubes": [ { "origin": [-4,7,-3], "size": [1,4,6], "uv": [24,13] } ] }
      ] } ] })";

void assertChickenDisk() {
    const SkeletalModel disk = SkeletalModel::parse(kChickenGeoJson, "geometry.chicken");
    const SkeletalModel baked = buildChicken();
    // Every 26.1 bone present and value-equal to the builder transcription.
    for (const char* bone : {"head", "beak", "redThing", "body", "rightLeg", "leftLeg",
                             "rightWing", "leftWing"}) {
        assert(disk.findBone(bone) >= 0);
        assertCubeEqual(*firstCube(disk, bone), *firstCube(baked, bone));
    }
    // beak/redThing hang off head (26.1 makes them head children).
    assert(disk.bones()[static_cast<std::size_t>(disk.findBone("beak"))].parent ==
           disk.findBone("head"));
    // 26.1 leg UV is (26,0), not the 1.16.1 (0,22); legs/wings never mirror.
    assert(vecEq(firstCube(disk, "rightLeg")->uv, glm::vec2{26.0F, 0.0F}));
    assert(!firstCube(disk, "leftLeg")->mirror);
    assert(!firstCube(disk, "leftWing")->mirror);
}

// The shipped resources/animation/zombie.geo.json (RN-1 adds the hat overlay).
constexpr std::string_view kZombieGeoJson = R"({
  "format_version": "1.12.0",
  "minecraft:geometry": [
    { "description": { "identifier": "geometry.zombie", "texture_width": 64, "texture_height": 64 },
      "bones": [
        { "name": "body", "pivot": [0,24,0], "cubes": [ { "origin": [-4,12,-2], "size": [8,12,4], "uv": [16,16] } ] },
        { "name": "head", "parent": "body", "pivot": [0,24,0], "cubes": [ { "origin": [-4,24,-4], "size": [8,8,8], "uv": [0,0] } ] },
        { "name": "hat", "parent": "head", "pivot": [0,24,0], "cubes": [ { "origin": [-4,24,-4], "size": [8,8,8], "uv": [32,0], "inflate": 0.5 } ] },
        { "name": "rightArm", "parent": "body", "pivot": [5,22,0], "cubes": [ { "origin": [4,12,-2], "size": [4,12,4], "uv": [40,16] } ] },
        { "name": "leftArm", "parent": "body", "pivot": [-5,22,0], "cubes": [ { "origin": [-8,12,-2], "size": [4,12,4], "uv": [40,16], "mirror": true } ] },
        { "name": "rightLeg", "pivot": [1.9,12,0], "cubes": [ { "origin": [-0.1,0,-2], "size": [4,12,4], "uv": [0,16] } ] },
        { "name": "leftLeg", "pivot": [-1.9,12,0], "cubes": [ { "origin": [-3.9,0,-2], "size": [4,12,4], "uv": [0,16], "mirror": true } ] }
      ] } ] })";

void assertZombieDisk() {
    const SkeletalModel disk = SkeletalModel::parse(kZombieGeoJson, "geometry.zombie");
    const SkeletalModel bakedHead = buildZombieHead();
    // The hat overlay exists, is a head child, and reproduces 26.1's values.
    assert(disk.findBone("hat") >= 0);
    assert(disk.bones()[static_cast<std::size_t>(disk.findBone("hat"))].parent ==
           disk.findBone("head"));
    const ModelCube* hat = firstCube(disk, "hat");
    assert(hat != nullptr);
    assert(near(hat->inflate, 0.5F));           // HumanoidModel hat = g.extend(0.5)
    assert(vecEq(hat->uv, glm::vec2{32.0F, 0.0F}));
    assert(vecEq(hat->size, glm::vec3{8.0F, 8.0F, 8.0F}));
    assertCubeEqual(*hat, *firstCube(bakedHead, "hat"));
    // The hat box coincides with the head box (only inflate/uv differ).
    assert(vecEq(hat->origin, firstCube(disk, "head")->origin));
}

} // namespace

int main() {
    // Golden: full cow, builder vs geo.json, value for value.
    const SkeletalModel jsonCow = SkeletalModel::parse(kCowGeoJson, "geometry.cow");
    const SkeletalModel bakedCow = buildCow();
    assertModelsEqual(bakedCow, jsonCow);

    // RN-1: species transcription guards (sheep double-layer, chicken 26.1
    // structure, zombie hat overlay) against the same builder+baker.
    assertSheepDisk();
    assertChickenDisk();
    assertZombieDisk();

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
