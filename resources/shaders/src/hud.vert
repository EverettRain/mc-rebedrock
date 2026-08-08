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

const vec2 blockUvs[18] = vec2[](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);

void main() {
    if (hud.data.x > 3.5 && hud.data.x < 4.5) {
        vec2 corner = blockCorners[gl_VertexIndex];
        gl_Position = vec4(hud.rect.xy + corner * hud.rect.zw, 0.0, 1.0);
        fragmentUv = blockUvs[gl_VertexIndex];
        fragmentTextureLayer = gl_VertexIndex < 6
            ? hud.data.y
            : (gl_VertexIndex < 12
                ? hud.data.z
                : (hud.data.x > 4.1 ? hud.data.w : hud.data.z));
        // Direction#getLuminance per face, applied as a plain scalar the way
        // vanilla 1.16.1's block item render does: up 1.0, west 0.6, east 0.8
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
