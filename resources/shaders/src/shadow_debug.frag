#version 450

layout(binding = 0) uniform sampler2D shadowDepth;

layout(location = 0) in vec2 fragmentUv;
layout(location = 0) out vec4 outColor;

void main() {
    // Raw depth is [0, 1] from the light's near plane; invert so near = bright.
    float depth = texture(shadowDepth, fragmentUv).r;
    outColor = vec4(vec3(1.0 - depth), 1.0);
}
