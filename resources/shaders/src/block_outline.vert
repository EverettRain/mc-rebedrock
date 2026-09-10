#version 450

#include "include/camera_uniform.glsl"

// Kept in lockstep with `struct OutlinePush` in src/render/vulkan/HudTypes.hpp by
// hud_push_constant_test, and filled only by makeOutlineSegmentPush.
layout(push_constant) uniform OutlinePush {
    vec4 blockOrigin;
    vec4 segmentStart;
    vec4 segmentEnd;
} outline;

// RN-45：vanilla 的描边不是线，是在顶点着色器里撑成的**屏幕空间四边形**
// （`rendertype_lines.vsh`）。整套理由与两处常量的出处见 BlockOutlineGeometry.hpp；
// 这里只记它换来的东西：线与面从此都是三角形，取的是同一批采样点，掠射的棱不再被
// 自己所在的面吃掉（RN-39），而 MSAA 下也不再是「每像素只盖住一个采样点」的半透明
// 细丝（RN-44）。
//
// 深度推近量是 vanilla 两项的乘积：`rendertype_lines.vsh` 自己的 1 - 1/256，
// 乘 LayeringTransform.VIEW_OFFSET_Z_LAYERING 的 1 - 1/4096。
const float kViewShrink = 0.9958505630;

// 六个顶点 = 两个三角形，索引序 0,1,2, 2,3,0，四个角是 (起点+, 起点-, 终点+, 终点-)。
// 两张表放在 main 外面：main 里不出现任何数字，block_outline_geometry 钉着这一条，
// 因为 RN-13-2 那个缺陷正是 main 里一个 1.02。
const float kQuadEndpoint[6] = float[6](0.0, 0.0, 1.0, 1.0, 1.0, 0.0);
const float kQuadSide[6] = float[6](1.0, -1.0, 1.0, 1.0, -1.0, 1.0);

void main() {
    // RN-16: one draw is one EDGE, not one box. The wireframe used to be twelve
    // edges generated from a box's two corners, drawn once per box of the
    // block's shape — which drew the seams where two boxes meet, and vanilla
    // draws none of those. `VoxelShape.forAllEdges` merges the boxes into one
    // occupancy grid first and emits only the segments where occupancy changes,
    // so the segment list is now built on the CPU (outlineEdgesOf) and each one
    // arrives here already in block-local coordinates.
    //
    // The endpoint is the coordinate, untouched — ShapeRenderer.renderShape
    // emits VoxelShape edge coordinates as they are. This used to expand the box
    // by 2% about its own centre, so the wireframe floated outside the block by
    // an amount that GREW with the box.
    vec3 local = mix(outline.segmentStart.xyz, outline.segmentEnd.xyz,
                     kQuadEndpoint[gl_VertexIndex]);
    vec3 edgeDirection = outline.segmentEnd.xyz - outline.segmentStart.xyz;
    vec3 worldPosition = outline.blockOrigin.xyz + local;
    // The line is separated from the surface in DEPTH instead, and in view space:
    // scaling the camera-relative position pulls the vertex toward the eye by a
    // fixed fraction of its distance, so the offset is the same for every box and
    // for every face. Nudging it outward in world space would instead bury a
    // shared edge inside the neighbouring block, behind that block's own face.
    vec4 viewPosition = camera.view * vec4(worldPosition, 1.0);
    vec4 viewAhead = camera.view * vec4(worldPosition + edgeDirection, 1.0);
    viewPosition.xyz *= kViewShrink;
    viewAhead.xyz *= kViewShrink;
    vec4 clipPosition = camera.projection * viewPosition;
    vec4 clipAhead = camera.projection * viewAhead;
    // 这条棱在屏幕上的方向，取它的法向推开半个线宽。两个端点各自算一次（透视下略有
    // 不同），符号统一靠 vanilla 那一手：offset.x 为负就整体取反，于是「正侧」在一条
    // 棱的两端是同一侧，四边形不会拧成沙漏
    vec2 screenSize = vec2(outline.segmentStart.w, outline.segmentEnd.w);
    vec2 screenDirection =
        normalize((clipAhead.xy / clipAhead.w - clipPosition.xy / clipPosition.w) * screenSize);
    vec2 offset = vec2(-screenDirection.y, screenDirection.x) *
                  outline.blockOrigin.w / screenSize;
    if (offset.x < 0.0) {
        offset = -offset;
    }
    vec2 ndc = clipPosition.xy / clipPosition.w + offset * kQuadSide[gl_VertexIndex];
    gl_Position = vec4(ndc * clipPosition.w, clipPosition.z, clipPosition.w);
}
