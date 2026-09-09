#version 450

// Sun-space depth-only pass: decodes the same packed VoxelVertex as
// grass_block.vert but projects through the light's view-projection instead of
// the camera. Everything arrives per-draw as push constants (light VP + section
// origin), so the pass needs no descriptor sets.

#include "include/shadow_map.glsl"
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

// 薄投射者（玻璃那一层）的**退化**门槛。
//
// ★ RN-55 更正 RN-51/52。这个门槛判的只有一件事：**这个面整个塌成一条线了吗**。
// 一个面在光空间的投影，沿它的面内投影方向被压缩 |N·L| 倍。面宽 1 格，所以投影宽度
// 是 `1 x |N·L|` 格；要它还能盖住采样点，就得 `|N·L| >= kMinTexels x 纹素`。
//
// 从前这里除的是**边框宽 1/16**，那等于把边框在投影里的宽度当成 `1/16 x |N·L|`。
// 错在：边框宽度的方向是**面内**的一个轴 W，它在投影里的压缩系数是
// `sqrt(1 - (W·L)^2)`，与 `|N·L|` 是两回事——两者在数值上毫无关系。
// 正午的南北向玻璃面 `|N·L| = 0.2696`，而它竖边框的投影宽度是**满的** 1/16 格
// （默认档 8 个纹素）。旧门槛（8/16/24 档 = 0.25/0.5/0.75）因此把这个**信号**
// 当噪声筛掉了：南北向的玻璃侧面阴影几乎全天消失，16/24 档更是一天都没有。
//
// 边框宽度够不够一个纹素是**另一条**判据，与朝向无关，由 RN-50「玻璃只进近段」
// 管着（远段一个纹素正好是 1/16 格，任何朝向都撑不住）。两条判据正交，
// 混成一个数正是上一轮的缺陷。
const float kThinCasterFaceBlocks = 1.0;
const float kThinCasterMinTexels = 2.0;

const float kLocalScale = 17.0 / 65535.0;
const float kUvScale = 2.0 / 65535.0;

void main() {
    uint posZ = inZNorm.x & 0xFFFFu;
    vec3 local = vec3(float(inPosXY.x), float(inPosXY.y), float(posZ)) * kLocalScale - vec3(0.5);
    vec3 world = shadow.sectionOrigin.xyz + local;
    gl_Position = shadow.lightViewProj * vec4(world, 1.0);
    fragmentUv = vec2(inUv) * kUvScale - vec2(0.5);
    fragmentTextureLayer = float(inLayerAO & 0xFFFFu);
    // ★ 整段包在这个分支里，而不是算完再按旗子取舍：这条通道是**顶点瓶颈**的，
    // 而绝大多数绘制不是薄投射者。旗子是逐绘制的推送常量，分支因此是 uniform 的，
    // 不会在波内分叉。
    fragmentCasterFacing = 1.0;
    if (shadow.sectionOrigin.w > 0.5) {
        // 光的方向从矩阵里取：正交投影的深度轴就是它（与 shadow_map.glsl 从矩阵推纹素
        // 是同一手），所以这里不需要多一个 uniform，也不可能与真正用的那张矩阵脱钩
        vec3 lightForward = normalize(vec3(shadow.lightViewProj[0][2], shadow.lightViewProj[1][2],
                                           shadow.lightViewProj[2][2]));
        vec3 normal = kVertexNormals[int(inZNorm.y & 0xFFu)];
        // 门槛随**这一级的纹素**走（RN-52 立的这条是对的，错的是被除的那个宽度）。
        // 面宽 1 格 ⇒ 三档分别是 0.0156 / 0.0312 / 0.0468——只有真正与光平行的面才落进去。
        float texelBlocks = sunShadowTexelBlocksOf(shadow.lightViewProj);
        float minFacing = kThinCasterMinTexels * texelBlocks / kThinCasterFaceBlocks;
        fragmentCasterFacing = abs(dot(normal, lightForward)) < minFacing ? 0.0 : 1.0;
    }
}
