#version 450

// Fullscreen-adjacent quad for the shadow-map debug overlay: the push constant
// carries a clip-space rect (x, y, width, height), the quad spans it, and the
// UVs map the whole depth texture.

layout(push_constant) uniform RectPush {
    vec4 rect;
} rect;

layout(location = 0) out vec2 fragmentUv;

const vec2 corners[6] = vec2[](
    vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
    vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0)
);

void main() {
    vec2 corner = corners[gl_VertexIndex];
    vec2 clip = rect.rect.xy + (corner * 0.5 + vec2(0.5)) * rect.rect.zw;
    gl_Position = vec4(clip, 0.0, 1.0);
    fragmentUv = corner * 0.5 + vec2(0.5);
}
