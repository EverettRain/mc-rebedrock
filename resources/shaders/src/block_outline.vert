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
// `modelViewStack.scale(1 - 1/4096)`, which RenderTypes.LINES applies through
// LayeringTransform.VIEW_OFFSET_Z_LAYERING. See BlockOutlineGeometry.hpp.
const float kViewShrink = 0.999755859375;

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
