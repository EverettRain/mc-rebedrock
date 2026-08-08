#version 450

// Sun-space depth-only pass: decodes the same packed VoxelVertex as
// grass_block.vert but projects through the light's view-projection instead of
// the camera. Everything arrives per-draw as push constants (light VP + section
// origin), so the pass needs no descriptor sets.

layout(push_constant) uniform ShadowPush {
    mat4 lightViewProj;
    vec4 sectionOrigin;
} shadow;

layout(location = 0) in uvec2 inPosXY;
layout(location = 1) in uvec2 inZNorm;
layout(location = 2) in uvec2 inUv;
layout(location = 3) in uint inLayerAO;
layout(location = 4) in uvec4 inLights;

const float kLocalScale = 17.0 / 65535.0;

void main() {
    uint posZ = inZNorm.x & 0xFFFFu;
    vec3 local = vec3(float(inPosXY.x), float(inPosXY.y), float(posZ)) * kLocalScale - vec3(0.5);
    vec3 world = shadow.sectionOrigin.xyz + local;
    gl_Position = shadow.lightViewProj * vec4(world, 1.0);
}
