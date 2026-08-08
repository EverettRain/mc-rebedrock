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

layout(location = 0) out vec3 worldDirection;
layout(location = 1) out vec2 screenNdc;

const vec2 fullscreenTriangle[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2(3.0, -1.0),
    vec2(-1.0, 3.0)
);

void main() {
    vec2 ndc = fullscreenTriangle[gl_VertexIndex];
    vec4 viewPosition = inverse(camera.projection) * vec4(ndc, 1.0, 1.0);
    vec3 viewDirection = normalize(viewPosition.xyz / viewPosition.w);
    worldDirection = transpose(mat3(camera.view)) * viewDirection;
    screenNdc = ndc;
    gl_Position = vec4(ndc, 1.0, 1.0);
}
