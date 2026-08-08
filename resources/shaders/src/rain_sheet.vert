#version 450

// 贴图雨 (texture rain): a few large camera-facing translucent quads sampling
// the water layer with a scrolling UV, so they read as falling rain bands.
// Push constants carry the sheet centre/size, scroll time, opacity and layer —
// no per-sheet descriptor state beyond the shared camera/texture set 0.

layout(binding = 0) uniform CameraUniform {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 cameraPosition;
    vec4 sunDirection;
    vec4 horizonFog;
    vec4 renderSettings;
} camera;

layout(push_constant) uniform RainSheetPush {
    vec4 positionSize;
    vec4 timeOpacityLayer;
} sheet;

layout(location = 0) out vec2 fragmentUv;
layout(location = 1) flat out float fragmentOpacity;
layout(location = 2) flat out float fragmentLayer;
layout(location = 3) flat out float fragmentTime;

const vec2 corners[6] = vec2[](
    vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.5, 0.5),
    vec2(-0.5, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5)
);

void main() {
    vec2 corner = corners[gl_VertexIndex];
    vec3 cameraRight = vec3(camera.view[0][0], camera.view[1][0], camera.view[2][0]);
    vec3 cameraUp = vec3(camera.view[0][1], camera.view[1][1], camera.view[2][1]);
    vec3 world = sheet.positionSize.xyz +
        (cameraRight * corner.x + cameraUp * corner.y) * sheet.positionSize.w;
    gl_Position = camera.projection * camera.view * vec4(world, 1.0);
    fragmentUv = corner + vec2(0.5);
    fragmentOpacity = sheet.timeOpacityLayer.y;
    fragmentLayer = sheet.timeOpacityLayer.z;
    fragmentTime = sheet.timeOpacityLayer.x;
}
