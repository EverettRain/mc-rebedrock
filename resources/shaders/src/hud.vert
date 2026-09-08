#version 450

// Declared identically in hud.frag and in mc::render::HudPush, and held together
// by hud_push_constant_test. A field means one thing in every draw mode; a mode
// may leave a field unused, but never reinterpret it. RN-14 put the icon's box
// into `color` and told only this shader, and every block icon in the inventory
// became a black diamond.
layout(push_constant) uniform HudPush {
    vec4 rect;       // clip-space origin xy, size zw
    vec4 color;      // tint, multiplied into the texel
    vec4 uvRect;     // sprite source origin xy, size zw (sprite modes)
    // x = draw mode, y = atlas layer.
    // UI-4: z = rotation in radians about the quad's own origin, w = the
    // framebuffer aspect (width/height) that makes that rotation isotropic in
    // PIXELS rather than in clip space. Both are zero in every other mode —
    // these are new meanings for components that never had one, not a
    // reinterpretation of an existing field (see the rule above).
    vec4 data;
    // The block icon draws ONE face of ONE box of the block's item model per
    // call. The box arrives in 0..1 cell coordinates, already turned into the
    // inventory pose; the four corner UVs are the model json's own rects sampled
    // by JE's rules and resolved on the CPU (mc::world::iconBoxOf) — which is why
    // this shader no longer carries a per-cube-model UV table for them to
    // disagree with.
    vec4 iconBoxMin; // xyz
    vec4 iconBoxMax; // xyz
    vec4 iconUv01;   // uv[0].xy, uv[1].xy
    vec4 iconUv23;   // uv[2].xy, uv[3].xy
} hud;

layout(location = 0) out vec2 fragmentUv;
layout(location = 1) flat out float fragmentTextureLayer;
// Per-vertex so the block icon's corner AO can shade the face with a gradient.
layout(location = 2) out vec3 fragmentLight;

const vec2 corners[6] = vec2[](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);

// Minecraft-style inventory block: the top diamond followed by the left and right
// faces. Keeping it procedural avoids a dedicated item mesh and vertex buffer.
//
// RN-14: these are the CUBE CORNERS the three visible faces stand on, not their
// projected screen positions. Until RN-14 this table held eighteen pre-projected
// 2D points, which is a unit cube and nothing else — a stair, a wall, a fence
// gate, a pressure plate and a button therefore all had to be drawn as full
// cubes, and that is what "a stair in my inventory looks like a plank" was. With
// the corners in 3D the same eighteen vertices draw any box, and a model is a
// list of boxes.
//
// Two triangles per face as (v0,v1,v2, v0,v2,v3). Mirrored by
// mc::world::kIconCubeCorners; item_cube_uv_test holds the two together.
const vec3 iconCorners[18] = vec3[](
    // top diamond = the up face
    vec3(1, 1, 1), vec3(0, 1, 1), vec3(0, 1, 0),
    vec3(1, 1, 1), vec3(0, 1, 0), vec3(1, 1, 0),
    // left parallelogram = the model's north face
    vec3(1, 1, 0), vec3(0, 1, 0), vec3(0, 0, 0),
    vec3(1, 1, 0), vec3(0, 0, 0), vec3(1, 0, 0),
    // right parallelogram = its west face
    vec3(0, 1, 0), vec3(0, 1, 1), vec3(0, 0, 1),
    vec3(0, 1, 0), vec3(0, 0, 1), vec3(0, 0, 0)
);

// Which of the face's four corners each of its six vertices stands on.
const int iconQuadCorner[6] = int[](0, 1, 2, 0, 2, 3);

// The orthographic isometric the icon is drawn in — vanilla's `gui` display
// transform, rotation [30, 225, 0]. Mirrored by mc::world::iconProject, which
// static_asserts that it reproduces the eighteen screen positions this table used
// to hold.
vec2 iconProject(vec3 p) {
    return vec2(0.5 + 0.44 * (p.z - p.x),
                0.46 - 0.21 * (p.x + p.z) + 0.48 * (1.0 - p.y));
}

// Depth along the view axis, normalised to [0,1]: the corner (0,1,0) is nearest
// the eye and (1,0,1) farthest. A multi-box model needs it — a wall's centre post
// and its arm interpenetrate, so no back-to-front ordering of whole boxes
// composites them correctly, and the icon pipeline therefore depth-tests.
float iconDepth(vec3 p) {
    return (p.x - p.y + p.z + 1.0) / 3.0;
}

void main() {
    if (hud.data.x > 3.5 && hud.data.x < 4.5) {
        // RN-14: one face of one box of the block's item model. The box arrives
        // in 0..1 cell coordinates already turned into the inventory pose (a
        // stair is drawn at vanilla's gui yaw 135, not the default 225 — at 225
        // it shows its plain back and reads as a plank), so all this does is map
        // the unit corner into the box and project it.
        vec3 unit = iconCorners[gl_VertexIndex];
        vec3 p = mix(hud.iconBoxMin.xyz, hud.iconBoxMax.xyz, unit);
        vec2 screen = iconProject(p);
        gl_Position = vec4(hud.rect.xy + screen * hud.rect.zw, iconDepth(p), 1.0);
        int corner = iconQuadCorner[gl_VertexIndex % 6];
        fragmentUv = corner == 0 ? hud.iconUv01.xy
                   : corner == 1 ? hud.iconUv01.zw
                   : corner == 2 ? hud.iconUv23.xy
                                 : hud.iconUv23.zw;
        fragmentTextureLayer = hud.data.y;
        // Direction#getLuminance per face, applied as a plain scalar the way
        // vanilla's block item render does: up 1.0, west 0.6, east 0.8
        // (no colour bias). A per-corner AO term darkens the silhouette edges
        // against the face centres so the cube reads rounded, like the smooth
        // lighting on vanilla item models.
        float faceLuminance = gl_VertexIndex < 6
            ? 1.0
            : (gl_VertexIndex < 12 ? 0.6 : 0.8);
        float cornerFactor = length(screen - vec2(0.5, 0.45));
        float ao = 1.0 - 0.13 * smoothstep(0.20, 0.50, cornerFactor);
        fragmentLight = vec3(faceLuminance * ao);
        return;
    }
    vec2 corner = corners[gl_VertexIndex];
    vec2 local = corner * hud.rect.zw;
    // 26.1's splash text is drawn through a rotated pose (SplashRenderer). Clip
    // space is anisotropic — one clip unit is W/2 pixels across and H/2 pixels
    // down — so rotating a clip-space offset directly would shear the glyphs.
    // Scaling x by the aspect puts both components in units of H/2, where the
    // rotation is a plain 2D rotation, and dividing back returns to clip space.
    if (hud.data.z != 0.0) {
        float aspect = hud.data.w;
        float c = cos(hud.data.z);
        float s = sin(hud.data.z);
        vec2 isotropic = vec2(local.x * aspect, local.y);
        vec2 rotated = vec2(isotropic.x * c - isotropic.y * s,
                            isotropic.x * s + isotropic.y * c);
        local = vec2(rotated.x / aspect, rotated.y);
    }
    gl_Position = vec4(hud.rect.xy + local, 0.0, 1.0);
    fragmentUv = hud.uvRect.xy + corner * hud.uvRect.zw;
    fragmentTextureLayer = hud.data.y;
    fragmentLight = vec3(1.0);
}
