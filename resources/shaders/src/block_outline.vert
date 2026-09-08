#version 450

layout(binding = 0) uniform CameraUniform {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 cameraPosition;
    vec4 sunDirection;
    vec4 horizonFog;
    vec4 renderSettings;
} camera;

// Kept in lockstep with `struct OutlinePush` in src/render/vulkan/HudTypes.hpp by
// hud_push_constant_test, and filled only by makeOutlineSegmentPush.
layout(push_constant) uniform OutlinePush {
    vec4 blockOrigin;
    vec4 segmentStart;
    vec4 segmentEnd;
} outline;

// RN-13-2. JE ProjectionType.PERSPECTIVE's layering transform at bias 1:
// RN-39: the fraction is 1/1024, not vanilla's 1/4096.
//
// The intent is vanilla's -- pull the line toward the eye by a fixed fraction of
// its camera distance -- but the number is not transferable. A line is
// rasterised at pixel centres that a triangle covering the same edge samples
// half a pixel away, and on a grazing face half a pixel of screen space is a
// depth step far larger than 1/4096 of the distance. The line then loses those
// pixels to the very face it sits on: measured off the export, the outline of a
// stone cube drew 824 of its 1194 line pixels at 1/4096 -- the edge facing the
// camera solid, the grazing ones dashed. That is the flicker.
//
// 1/1024 saturates it (1187 pixels; 1/512 and 1/256 add three and six more,
// which is antialiasing noise). Four times vanilla's offset is 0.004 blocks at
// four blocks out, far under a pixel at any reasonable resolution, so the line
// does not visibly float off the surface.
//
// `modelViewStack.scale(1 - 1/4096)`, which RenderTypes.LINES applies through
// LayeringTransform.VIEW_OFFSET_Z_LAYERING. See BlockOutlineGeometry.hpp.
const float kViewShrink = 0.9990234375;

void main() {
    // RN-16: one draw is one LINE, not one box. The wireframe used to be twelve
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
    vec3 local = gl_VertexIndex == 0 ? outline.segmentStart.xyz : outline.segmentEnd.xyz;
    vec3 worldPosition = outline.blockOrigin.xyz + local;
    // The line is separated from the surface in DEPTH instead, and in view space:
    // scaling the camera-relative position pulls the vertex toward the eye by a
    // fixed fraction of its distance, so the offset is the same for every box and
    // for every face. Nudging it outward in world space would instead bury a
    // shared edge inside the neighbouring block, behind that block's own face.
    vec4 viewPosition = camera.view * vec4(worldPosition, 1.0);
    viewPosition.xyz *= kViewShrink;
    gl_Position = camera.projection * viewPosition;
}
