#version 450

layout(push_constant) uniform HudPush {
    vec4 rect;
    vec4 color;
    vec4 uvRect;
    vec4 data;
} hud;

layout(location = 0) out vec2 fragmentUv;
layout(location = 1) flat out float fragmentTextureLayer;
layout(location = 2) flat out vec3 fragmentLight;

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
        // Match the two-light GUI setup used by Minecraft's item renderer:
        // a bright overhead key, a cool dark left face and a warm right fill.
        fragmentLight = gl_VertexIndex < 6
            ? vec3(1.02, 1.02, 1.00)
            : (gl_VertexIndex < 12
                ? vec3(0.58, 0.61, 0.66)
                : vec3(0.80, 0.77, 0.70));
        return;
    }
    vec2 corner = corners[gl_VertexIndex];
    gl_Position = vec4(hud.rect.xy + corner * hud.rect.zw, 0.0, 1.0);
    fragmentUv = hud.uvRect.xy + corner * hud.uvRect.zw;
    fragmentTextureLayer = hud.data.y;
    fragmentLight = vec3(1.0);
}
