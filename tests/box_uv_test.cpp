#include "animation/SkeletalModel.hpp"

#include <cassert>
#include <cmath>
#include <utility>

namespace {

bool near(float a, float b) { return std::abs(a - b) < 1e-4F; }

bool rect(const mc::animation::BoxUvRect& r, float ox, float oy, float sx, float sy) {
    return near(r.origin.x, ox) && near(r.origin.y, oy) && near(r.size.x, sx) &&
           near(r.size.y, sy);
}

} // namespace

int main() {
    using mc::animation::boxUvFaceRect;

    // The quadruped body cube: uv [28, 8], size [8, 8, 16] (sx=8, sy=8, sz=16).
    // Verify every face rect against the standard box-UV net so the shader (which
    // mirrors this) and the painted skin stay aligned.
    const glm::vec2 uv{28.0F, 8.0F};
    const glm::vec3 size{8.0F, 8.0F, 16.0F};

    assert(rect(boxUvFaceRect(0, uv, size), 28.0F + 16.0F + 8.0F, 8.0F + 16.0F, 16.0F, 8.0F)); // +X
    assert(rect(boxUvFaceRect(1, uv, size), 28.0F, 8.0F + 16.0F, 16.0F, 8.0F));                // -X
    // Vanilla ModelPart.Cube (ModelPart.java:297-303): -Y (down) is the LEFT
    // top-row rect (u1..u2), +Y (up) the RIGHT (u2..u22).
    assert(rect(boxUvFaceRect(2, uv, size), 28.0F + 16.0F + 8.0F, 8.0F, 8.0F, 16.0F));         // +Y up (right)
    assert(rect(boxUvFaceRect(3, uv, size), 28.0F + 16.0F, 8.0F, 8.0F, 16.0F));                // -Y down (left)
    // -Z is the front (second middle-row rect); +Z is the back (fourth rect).
    assert(rect(boxUvFaceRect(4, uv, size), 28.0F + 32.0F + 8.0F, 8.0F + 16.0F, 8.0F, 8.0F));  // +Z back
    assert(rect(boxUvFaceRect(5, uv, size), 28.0F + 16.0F, 8.0F + 16.0F, 8.0F, 8.0F));         // -Z front

    // Top-row caps (-Y, +Y) tile side by side above the middle row: down (left)
    // sits immediately left of up (right), matching vanilla's u1..u2 / u2..u22.
    const auto up = boxUvFaceRect(2, uv, size);
    const auto down = boxUvFaceRect(3, uv, size);
    assert(near(down.origin.x + down.size.x, up.origin.x));

    // The four middle-row side faces tile without gaps or overlaps across the
    // net width 2*(sx+sz): order is -X, -Z(front), +X, +Z(back).
    const auto west = boxUvFaceRect(1, uv, size);
    const auto front = boxUvFaceRect(5, uv, size);
    const auto east = boxUvFaceRect(0, uv, size);
    const auto back = boxUvFaceRect(4, uv, size);
    assert(near(west.origin.x + west.size.x, front.origin.x));
    assert(near(front.origin.x + front.size.x, east.origin.x));
    assert(near(east.origin.x + east.size.x, back.origin.x));
    assert(near(back.origin.x + back.size.x - uv.x, 2.0F * (size.x + size.z)));

    // A cube with a base uv of 0 keeps the net anchored at the origin.
    const auto leg = boxUvFaceRect(1, {0.0F, 16.0F}, {4.0F, 6.0F, 4.0F});
    assert(rect(leg, 0.0F, 16.0F + 4.0F, 4.0F, 6.0F));

    // Golden vanilla ModelPart.Cube cross-check (ModelPart.java:287-303). Compute
    // the vanilla texel offsets directly from a transcribed cube's declared size
    // and assert our up/down cap rects land on the same axis-aligned region.
    // vanilla: u1 = x+depth, u2 = x+depth+width, u22 = x+depth+width+width;
    //          v0 = y, v1 = y+depth.
    //   DOWN polygon spans (u1..u2) x (v0..v1)  -> left cap.
    //   UP   polygon spans (u2..u22) x (v1..v0) -> right cap (v flipped, same rect).
    {
        // Two differently-shaped transcribed cubes to pin the formula, not a
        // single memorised constant.
        const glm::vec2 goldenUv{18.0F, 4.0F};
        const glm::vec3 goldenSize{12.0F, 18.0F, 10.0F}; // sx=12, sy=18, sz=10
        for (const auto& [gUv, gSize] :
             {std::pair{uv, size}, std::pair{goldenUv, goldenSize}}) {
            const float gx = gUv.x, gy = gUv.y;
            const float gsx = gSize.x, gsz = gSize.z; // width, depth
            const float u1 = gx + gsz;
            const float u2 = gx + gsz + gsx;
            const float u22 = gx + gsz + gsx + gsx;
            const float v0 = gy;
            const float v1 = gy + gsz;
            // DOWN == vanilla left cap (u1..u2, v0..v1).
            assert(rect(boxUvFaceRect(3, gUv, gSize), u1, v0, u2 - u1, v1 - v0));
            // UP == vanilla right cap (u2..u22, same height span).
            assert(rect(boxUvFaceRect(2, gUv, gSize), u2, v0, u22 - u2, v1 - v0));
        }
    }

    // Bedrock euler order is Z, then Y, then X (matrix Rz * Ry * Rx), so X is
    // applied first: (90, 90, 0) sends +Z to -Y, and Y leaves it there. The
    // opposite composition would land on +X. tools/entity_uv_lib.py's selftest
    // pins this exact vector, which is what keeps the offline preview tools
    // posing multi-axis bones the way the game does.
    const glm::vec3 turned =
        glm::vec3{mc::animation::rotationMatrix({90.0F, 90.0F, 0.0F}) *
                  glm::vec4{0.0F, 0.0F, 1.0F, 0.0F}};
    assert(near(turned.x, 0.0F) && near(turned.y, -1.0F) && near(turned.z, 0.0F));

    // Rotating about a pivot leaves the pivot itself fixed.
    const glm::vec3 pivot{3.0F, 6.0F, -5.0F};
    const glm::vec3 anchored = glm::vec3{
        mc::animation::rotationAboutPivot({0.0F, 37.0F, 0.0F}, pivot) * glm::vec4{pivot, 1.0F}};
    assert(near(anchored.x, pivot.x) && near(anchored.y, pivot.y) && near(anchored.z, pivot.z));

    // `inflate` grows the drawn box around its centre without moving the box-UV
    // net: the rects a cube samples are the ones its declared size implies.
    mc::animation::ModelCube cube;
    cube.origin = {-2.0F, 9.0F, -15.0F};
    cube.size = {4.0F, 3.0F, 1.0F};
    cube.inflate = 0.5F;
    assert(near(cube.renderSize().x, 5.0F) && near(cube.renderSize().y, 4.0F) &&
           near(cube.renderSize().z, 2.0F));
    assert(near(cube.center().x, 0.0F) && near(cube.center().y, 10.5F) &&
           near(cube.center().z, -14.5F));
    const auto inflatedFront = boxUvFaceRect(5, {16.0F, 16.0F}, cube.size);
    assert(rect(inflatedFront, 16.0F + 1.0F, 16.0F + 1.0F, 4.0F, 3.0F));

    // The "faces" extension (the texture editor's per-rect relabel/rotate) packs
    // into ModelCube::faceOverride: 4 bits per face (0..5 = +X,-X,+Y,-Y,+Z,-Z),
    // low 3 bits = source net-position rect, bit 3 = 180° rotation. Identity
    // (every face samples its own rect, no rotation) is 0x00543210. The shader
    // unpacks the same layout from textureLayersRotation.w.
    using mc::animation::SkeletalModel;
    const auto overridden = SkeletalModel::parse(R"({
      "format_version": "1.12.0",
      "minecraft:geometry": [
        { "description": {"identifier": "geometry.t", "texture_width": 64, "texture_height": 32},
          "bones": [
            {"name": "b", "cubes": [
              {"origin": [0,0,0], "size": [8,8,8], "uv": [0,0],
               "faces": {"back": {"as": "front", "rotate": 180},
                         "up": "down"}}
            ]}
          ]
        }
      ]
    })").bones()[0].cubes[0].faceOverride;
    // "back"(4) serves front(5) rotated 180 -> face 5 nibble 0xC; "up"(2) serves
    // down(3) -> face 3 nibble 0x2.
    assert(overridden == 0x00C42210U);

    // A cube without the extension keeps the identity packing.
    assert(SkeletalModel::parse(R"({
      "format_version": "1.12.0",
      "minecraft:geometry": [
        { "description": {"identifier": "geometry.i", "texture_width": 64, "texture_height": 32},
          "bones": [ {"name": "b", "cubes": [{"origin": [0,0,0], "size": [8,8,8], "uv": [0,0]}]} ] }
      ]
    })").bones()[0].cubes[0].faceOverride == 0x00543210U);

    // Unknown face names in the extension are ignored, not fatal.
    assert(SkeletalModel::parse(R"({
      "format_version": "1.12.0",
      "minecraft:geometry": [
        { "description": {"identifier": "geometry.j", "texture_width": 64, "texture_height": 32},
          "bones": [ {"name": "b", "cubes": [{"origin": [0,0,0], "size": [8,8,8], "uv": [0,0],
            "faces": {"north": {"as": "front", "rotate": 180}}} ]} ] }
      ]
    })").bones()[0].cubes[0].faceOverride == 0x00543210U);
    return 0;
}
