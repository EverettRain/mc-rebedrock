#version 450

layout(location = 0) in vec2 fragmentUv;
layout(location = 1) flat in float fragmentTextureLayer;
layout(location = 5) flat in float fragmentOpacity;
layout(location = 6) in float fragmentCameraDistance;
layout(location = 8) flat in vec2 fragmentSceneLight;
layout(location = 9) in vec3 fragmentWorldPosition;
layout(location = 0) out vec4 outColor;

// The full camera block: the tail (point lights, lighting settings) is the same
// buffer grass_block.frag reads, so particles can use the terrain's lighting
// terms verbatim.
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

layout(binding = 1) uniform sampler2DArray blockTextures;

// Vanilla's light curve, identical to grass_block.frag / item_entity.frag.
float lightBrightness(float normalizedLevel) {
    float darkness = 1.0 - clamp(normalizedLevel, 0.0, 1.0);
    return normalizedLevel / (darkness * 3.0 + 1.0);
}

void main() {
    if (fragmentOpacity < 0.01) {
        discard;
    }
    vec4 texel = texture(blockTextures, vec3(fragmentUv, fragmentTextureLayer));
    if (texel.a < 0.1) {
        discard;
    }
    if (fragmentSceneLight.x >= 0.0) {
        // Same curve, colours and moving point lights as grass_block.frag, so a
        // particle reads as part of the scene it stands in: sky light follows
        // the day/night cycle, block light keeps the warm torch tint, and the
        // two combine with max() rather than summing.
        vec3 skyTint = mix(vec3(0.50, 0.62, 0.95), vec3(1.0, 0.97, 0.90),
                           camera.sunDirection.w);
        float skyBrightness = lightBrightness(fragmentSceneLight.x) * camera.sunDirection.w;
        float blockBrightness = lightBrightness(fragmentSceneLight.y);
        vec3 illumination = max(skyTint * vec3(skyBrightness),
                                vec3(1.0, 0.72, 0.38) * blockBrightness * 0.92);
        for (int lightIndex = 0; lightIndex < int(camera.lightingSettings.x); ++lightIndex) {
            vec3 delta = camera.pointLights[lightIndex].xyz - fragmentWorldPosition;
            float radius = camera.pointLights[lightIndex].w;
            float attenuation = pow(max(1.0 - length(delta) / radius, 0.0), 2.0);
            illumination += camera.lightColors[lightIndex].rgb * attenuation *
                camera.lightColors[lightIndex].a;
        }
        illumination = clamp(illumination, vec3(0.035), vec3(1.25));
        texel.rgb *= illumination;
    }
    texel.a *= fragmentOpacity;
    if (camera.renderSettings.y > 0.5) {
        float densityDistance = fragmentCameraDistance * camera.renderSettings.z;
        float fog = max(
            1.0 - exp(-(densityDistance * densityDistance)),
            smoothstep(
                camera.renderSettings.w * 0.25,
                camera.renderSettings.w,
                fragmentCameraDistance));
        texel.rgb = mix(texel.rgb, vec3(0.0196, 0.0196, 0.20), clamp(fog, 0.0, 1.0));
    } else if (fragmentSceneLight.x >= 0.0) {
        // Scene-lit draws also take the terrain's horizon fog, so particles at
        // the far edge of the render distance fade into the sky the same way
        // the chunks around them do.
        float fogEnd = max(camera.renderSettings.x, 16.0);
        float fog = smoothstep(fogEnd * 0.75, fogEnd, fragmentCameraDistance);
        texel.rgb = mix(texel.rgb, camera.horizonFog.rgb, clamp(fog, 0.0, 1.0));
    }
    outColor = texel;
}
