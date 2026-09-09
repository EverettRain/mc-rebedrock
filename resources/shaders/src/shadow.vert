#version 450

// Sun-space depth-only pass: decodes the same packed VoxelVertex as
// grass_block.vert but projects through the light's view-projection instead of
// the camera. Everything arrives per-draw as push constants (light VP + section
// origin), so the pass needs no descriptor sets.

#include "include/vertex_normals.glsl"

layout(push_constant) uniform ShadowPush {
    mat4 lightViewProj;
    // RN-51：`.w` 是「这一趟画的是薄投射者吗」（玻璃那一层）。ShadowPush 的这个分量
    // 一直是未赋义的，与 RN-45 用 OutlinePush 的 w 分量是同一个办法。
    vec4 sectionOrigin;
} shadow;

layout(location = 0) in uvec2 inPosXY;
layout(location = 1) in uvec2 inZNorm;
layout(location = 2) in uvec2 inUv;
layout(location = 3) in uint inLayerAO;
layout(location = 4) in uvec4 inLights;

// The cutout variant of the pass needs the atlas coordinate to alpha-test with.
// They are written unconditionally: shadow.frag simply ignores them, and a
// vertex stage is allowed to produce outputs the fragment stage never reads.
// One vertex shader for both pipelines keeps the position maths in one place —
// two copies of it drifting is how a shadow ends up offset from its caster.
layout(location = 0) out vec2 fragmentUv;
layout(location = 1) flat out float fragmentTextureLayer;
// RN-51：这个面正对光的程度 |N·L|，**只有薄投射者**才是真值，其余一律 1（不筛）。
//
// 玻璃的边框宽 1/16 格。一个近乎侧对光的面，它挡光的投影宽度是 1/16 x |N·L|——
// |N·L| = 0.1 时只有 0.006 格，连近段一个纹素（1/128 格）都盖不满。渲进阴影图的
// 因此不是信号是噪声：纹素中心落在边框内外全看运气，实机上是一条断断续续的发丝影，
// 用户两次报的「阴影线条上的光斑」就是它。
//
// 物理上这也是对的：侧对光的薄片挡住的光正比于 |N·L|，趋于零。
layout(location = 2) flat out float fragmentCasterFacing;

const float kLocalScale = 17.0 / 65535.0;
const float kUvScale = 2.0 / 65535.0;

void main() {
    uint posZ = inZNorm.x & 0xFFFFu;
    vec3 local = vec3(float(inPosXY.x), float(inPosXY.y), float(posZ)) * kLocalScale - vec3(0.5);
    vec3 world = shadow.sectionOrigin.xyz + local;
    gl_Position = shadow.lightViewProj * vec4(world, 1.0);
    fragmentUv = vec2(inUv) * kUvScale - vec2(0.5);
    fragmentTextureLayer = float(inLayerAO & 0xFFFFu);
    // 光的方向从矩阵里取：正交投影的深度轴就是它（与 sun_shadow.glsl 从矩阵推纹素
    // 是同一手），所以这里不需要多一个 uniform，也不可能与真正用的那张矩阵脱钩
    vec3 lightForward = normalize(vec3(shadow.lightViewProj[0][2], shadow.lightViewProj[1][2],
                                       shadow.lightViewProj[2][2]));
    vec3 normal = kVertexNormals[int(inZNorm.y & 0xFFu)];
    fragmentCasterFacing =
        shadow.sectionOrigin.w > 0.5 ? abs(dot(normal, lightForward)) : 1.0;
}
