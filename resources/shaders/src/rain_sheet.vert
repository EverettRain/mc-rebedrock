#version 450

// Vanilla precipitation column. Each draw is one block-wide vertical
// strip whose horizontal tangent faces the camera radially. The original
// 64x256 environment/rain.png repeats every four blocks and scrolls downward;
// it is not a water tile stretched over a camera-facing square.

layout(binding = 0) uniform CameraUniform {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 cameraPosition;
    vec4 sunDirection;
    vec4 horizonFog;
    vec4 renderSettings;
} camera;

struct RainColumn {
    vec4 positionBottomWidth;
    vec4 topOpacityPhaseLight;
    vec4 tangent;
};

layout(std430, set = 1, binding = 0) readonly buffer RainColumns {
    RainColumn records[];
} columns;

layout(location = 0) out vec2 fragmentUv;
layout(location = 1) flat out float fragmentOpacity;
layout(location = 2) flat out vec2 fragmentSceneLight;

// x is the signed half-width and y selects the bottom/top of the column.
const vec2 corners[6] = vec2[](
    vec2(-1.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(-1.0, 0.0), vec2(1.0, 1.0), vec2(-1.0, 1.0)
);

vec2 decodeSceneLight(float packedLight) {
    float value = max(packedLight - 1.0, 0.0);
    return vec2(mod(value, 16.0), floor(value / 16.0)) / 15.0;
}

void main() {
    RainColumn sheet = columns.records[gl_InstanceIndex];
    vec2 corner = corners[gl_VertexIndex];
    vec2 horizontal = sheet.tangent.xy * corner.x * sheet.positionBottomWidth.w;
    float worldY = mix(sheet.positionBottomWidth.y, sheet.topOpacityPhaseLight.x, corner.y);
    vec3 world = vec3(sheet.positionBottomWidth.x + horizontal.x,
                      worldY,
                      sheet.positionBottomWidth.z + horizontal.y);
    gl_Position = camera.projection * camera.view * vec4(world, 1.0);
    // The original top vertices use bottomY*0.25 and the bottom vertices use
    // topY*0.25. Preserve that V orientation before applying its negative
    // per-column tick scroll.
    float mirroredY = sheet.positionBottomWidth.y + sheet.topOpacityPhaseLight.x - worldY;
    fragmentUv = vec2(corner.x * 0.5 + 0.5,
                      mirroredY * 0.25 + sheet.topOpacityPhaseLight.z);
    fragmentOpacity = sheet.topOpacityPhaseLight.y;
    fragmentSceneLight = decodeSceneLight(sheet.topOpacityPhaseLight.w);
}
