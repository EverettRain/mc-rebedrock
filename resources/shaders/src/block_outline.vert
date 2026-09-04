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

layout(push_constant) uniform OutlinePush {
    vec4 blockOrigin;
    vec4 boundsMin;
    vec4 boundsMax;
} outline;

// Kept in lockstep with src/render/BlockOutlineGeometry.hpp by
// block_outline_geometry_test — the shader has no vertex buffer, so these two
// tables and the constant below are the whole definition of the wireframe.
const vec3 corners[8] = vec3[](
    vec3(0.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0),
    vec3(1.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 1.0),
    vec3(1.0, 1.0, 1.0), vec3(0.0, 1.0, 1.0)
);

const int edgeVertices[24] = int[](
    0, 1, 1, 2, 2, 3, 3, 0,
    4, 5, 5, 6, 6, 7, 7, 4,
    0, 4, 1, 5, 2, 6, 3, 7
);

// RN-13-2. JE ProjectionType.PERSPECTIVE's layering transform at bias 1:
// `modelViewStack.scale(1 - 1/4096)`, which RenderTypes.LINES applies through
// LayeringTransform.VIEW_OFFSET_Z_LAYERING. See BlockOutlineGeometry.hpp.
const float kViewShrink = 0.999755859375;

void main() {
    // The endpoint is a corner of the box, untouched — ShapeRenderer.renderShape
    // emits VoxelShape edge coordinates as they are. This used to expand the box
    // by 2% about its own centre, so the wireframe floated outside the block by
    // an amount that GREW with the box: 0.01 blocks around a full cube, an
    // eighth of that around a diode's 2px base. RN-10f put boxes of both sizes
    // on screen at once and the inconsistency became visible.
    vec3 unit = corners[edgeVertices[gl_VertexIndex]];
    vec3 local = mix(outline.boundsMin.xyz, outline.boundsMax.xyz, unit);
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
