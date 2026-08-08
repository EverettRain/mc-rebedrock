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

void main() {
    // Map the unit-cube corner into the block's actual bounds, then expand
    // slightly outward from the box centre so the wireframe hugs the shape.
    vec3 unit = corners[edgeVertices[gl_VertexIndex]];
    vec3 local = mix(outline.boundsMin.xyz, outline.boundsMax.xyz, unit);
    vec3 boxCenter = 0.5 * (outline.boundsMin.xyz + outline.boundsMax.xyz);
    local = boxCenter + (local - boxCenter) * 1.02;
    vec3 worldPosition = outline.blockOrigin.xyz + local;
    gl_Position = camera.projection * camera.view * vec4(worldPosition, 1.0);
}
