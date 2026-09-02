#version 450

layout(push_constant) uniform HudPush {
    vec4 rect;
    vec4 color;
    vec4 uvRect;
    vec4 data;
} hud;

layout(location = 0) out vec2 fragmentUv;
layout(location = 1) flat out float fragmentTextureLayer;
// Per-vertex so the block icon's corner AO can shade the face with a gradient.
layout(location = 2) out vec3 fragmentLight;

const vec2 corners[6] = vec2[](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);

// Minecraft-style inventory block: top diamond followed by the left and right
// faces. Keeping it procedural avoids a dedicated item mesh and vertex buffer.
const vec2 blockCorners[18] = vec2[](
    vec2(0.50, 0.04), vec2(0.94, 0.25), vec2(0.50, 0.46),
    vec2(0.50, 0.04), vec2(0.50, 0.46), vec2(0.06, 0.25),
    vec2(0.06, 0.25), vec2(0.50, 0.46), vec2(0.50, 0.94),
    vec2(0.06, 0.25), vec2(0.50, 0.94), vec2(0.06, 0.73),
    vec2(0.50, 0.46), vec2(0.94, 0.25), vec2(0.94, 0.73),
    vec2(0.50, 0.46), vec2(0.94, 0.73), vec2(0.50, 0.94)
);

// RN-8c: the icon is a 3D render of the block's own model, so each visible face
// must sample the sprite exactly as that world face does. vanilla's `gui` item
// transform is rotation [30, 225, 0], which shows three faces: the up face as the
// top diamond, the model's NORTH face as the left parallelogram and its WEST face
// as the right one. (That pairing is why vanilla's crafting_table.json puts
// #front on north *and* west — the icon shows the front on both visible sides.)
//
// Working the projection through, the diamond's four vertices are the cube's four
// top corners: bottom = (0,1,0) (the nearest one), right = (0,1,1),
// top = (1,1,1) (the farthest), left = (1,1,0). With the up face reading
// u = x, v = z (JE defaultFaceUV), that fixes the diamond's UVs.
//
// The two side faces were already right. The top diamond was 180 degrees out,
// which is invisible on a symmetric sprite and plain on a crafting table, an
// observer or a hay bale — the "directional blocks' thumbnails are still
// rotated" report. These 18 values are checked against mc::world::kCubeItemFaceUv
// by item_cube_uv_test.
// ---- kCubeItemFaceUv icon begin ----
// Three tables of 18, one per cube model, indexed [model * 18 + vertex]. Which
// one a block uses is its declared CubeUvModel, pushed in hud.uvRect.y:
// 0 = Default (block/cube), 1 = PistonTemplate (template_piston.json rotates its
// down/west/east faces), 2 = Observer (observer.json declares an inverted rect on
// its up face). Generated from mc::world::kCubeModelFaceUv and checked against it
// by item_cube_uv_test.
const vec2 blockUvs[54] = vec2[](
    // --- Default
    vec2(1.0, 1.0), vec2(0.0, 1.0), vec2(0.0, 0.0),
    vec2(1.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 0.0),
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0),
    // --- PistonTemplate
    vec2(1.0, 1.0), vec2(0.0, 1.0), vec2(0.0, 0.0),
    vec2(1.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 0.0),
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0),
    vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0),
    vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, 0.0),
    // --- Observer
    vec2(1.0, 0.0), vec2(0.0, 0.0), vec2(0.0, 1.0),
    vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);
// ---- kCubeItemFaceUv icon end ----

void main() {
    if (hud.data.x > 3.5 && hud.data.x < 4.5) {
        vec2 corner = blockCorners[gl_VertexIndex];
        // Slab icon: the block is drawn half height. hud.uvRect.x carries the
        // portion (0 = full cube, 1 = bottom half, 2 = top half); the block
        // branch never reads uvRect otherwise. One block's side rises 0.48 of the
        // icon on screen, so folding the top edge (or the bottom edge) down by
        // half of that collapses the cube to the matching half slab.
        float portion = hud.uvRect.x;
        int index = gl_VertexIndex;
        bool bottomEdge = index == 8 || index == 10 || index == 11 ||
                          index == 14 || index == 16 || index == 17;
        const float halfBlock = 0.24;
        if (portion > 0.5 && portion < 1.5 && !bottomEdge) {
            corner.y += halfBlock; // bottom slab: drop the top face to mid height
        } else if (portion > 1.5 && bottomEdge) {
            corner.y -= halfBlock; // top slab: lift the bottom edge to mid height
        }
        gl_Position = vec4(hud.rect.xy + corner * hud.rect.zw, 0.0, 1.0);
        // A slab icon's two side faces show only the lower half strip of the side
        // sprite (v in [0.5, 1]), the way vanilla's slab model maps a 16x8 side —
        // the same crop the dropped and held slab already do in item_entity.vert.
        // Without it the whole texture was squeezed into the half-height box.
        int uvModel = int(hud.uvRect.y + 0.5);
        vec2 iconUv = blockUvs[uvModel * 18 + gl_VertexIndex];
        if (gl_VertexIndex >= 6) {
            if (portion > 0.5 && portion < 1.5) {
                iconUv.y = 0.5 + iconUv.y * 0.5; // bottom slab: lower half
            } else if (portion > 1.5) {
                iconUv.y = iconUv.y * 0.5;       // top slab: upper half
            }
        }
        fragmentUv = iconUv;
        fragmentTextureLayer = gl_VertexIndex < 6
            ? hud.data.y
            : (gl_VertexIndex < 12
                ? hud.data.z
                : (hud.data.x > 4.1 ? hud.data.w : hud.data.z));
        // Direction#getLuminance per face, applied as a plain scalar the way
        // vanilla's block item render does: up 1.0, west 0.6, east 0.8
        // (no colour bias). A per-corner AO term darkens the silhouette edges
        // against the face centres so the cube reads rounded, like the smooth
        // lighting on vanilla item models.
        float faceLuminance = gl_VertexIndex < 6
            ? 1.0
            : (gl_VertexIndex < 12 ? 0.6 : 0.8);
        float cornerFactor = length(corner - vec2(0.5, 0.45));
        float ao = 1.0 - 0.13 * smoothstep(0.20, 0.50, cornerFactor);
        fragmentLight = vec3(faceLuminance * ao);
        return;
    }
    vec2 corner = corners[gl_VertexIndex];
    gl_Position = vec4(hud.rect.xy + corner * hud.rect.zw, 0.0, 1.0);
    fragmentUv = hud.uvRect.xy + corner * hud.uvRect.zw;
    fragmentTextureLayer = hud.data.y;
    fragmentLight = vec3(1.0);
}
