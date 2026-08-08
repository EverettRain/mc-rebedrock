#version 450

// Occlusion query pass: expands a unit cube to a section's AABB and draws it
// with color and depth writes disabled, counting only the fragments that pass
// the depth test against the terrain already drawn this frame. A nonzero count
// means part of the section is in front of the scene, so it stays visible;
// a zero count means the section is buried and its mesh can be culled.
layout(binding = 0) uniform CameraUniform {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 cameraPosition;
    vec4 sunDirection;
    vec4 horizonFog;
    vec4 renderSettings;
    vec4 pointLights[8];
    vec4 lightColors[8];
    vec4 lightingSettings;
} camera;

layout(push_constant) uniform PushConstants {
    vec4 aabbMinimum;
    vec4 aabbMaximum;
} push;

layout(location = 0) in vec3 inPosition;

void main() {
    vec3 world = mix(push.aabbMinimum.xyz, push.aabbMaximum.xyz, inPosition);
    gl_Position = camera.projection * camera.view * vec4(world, 1.0);
}
