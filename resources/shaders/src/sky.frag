#version 450

layout(location = 0) in vec3 worldDirection;
layout(location = 1) in vec2 screenNdc;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform CameraUniform {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 cameraPosition;
    vec4 sunDirection;
    vec4 horizonFog;
    vec4 renderSettings;
} camera;

layout(binding = 1) uniform sampler2DArray blockTextures;

const vec3 dayZenithColor = vec3(0.20, 0.46, 0.82);
const vec3 nightZenithColor = vec3(0.004, 0.008, 0.030);

// Layer 137 is 1.16.1 environment/sun.png; layers 224..231 are the eight
// moon_phases.png tiles. Keep in sync with kMoonPhaseFirstLayer in the renderer.
const float sunLayer = 137.0;
const float moonFirstLayer = 224.0;

// Project a celestial direction to screen NDC and return its sprite colour
// (texel.rgb premultiplied by alpha, i.e. vanilla SRC_ALPHA/ONE additive), or
// zero when the body is below the horizon or the fragment is outside the quad.
// Evaluating the sprite directly in NDC keeps all four edges straight at every
// pitch, unlike ray/sphere intersection which can bow the disc at the corners.
vec3 celestialSprite(vec3 direction, float layer, float halfHeightNdc) {
    vec4 clip = camera.projection * mat4(mat3(camera.view)) * vec4(direction, 0.0);
    if (clip.w <= 0.0 || direction.y < -0.05) {
        return vec3(0.0);
    }
    vec2 center = clip.xy / clip.w;
    float aspect = abs(camera.projection[1][1] / camera.projection[0][0]);
    vec2 halfSize = vec2(halfHeightNdc / aspect, halfHeightNdc);
    vec2 uv = (screenNdc - center) / (halfSize * 2.0) + vec2(0.5);
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return vec3(0.0);
    }
    vec4 texel = texture(blockTextures, vec3(uv, layer));
    return texel.rgb * texel.a;
}

// Vanilla getMoonPhase(): (worldTime / 24000) % 8. worldTime starts at tick 6000
// and advances at 20 ticks per second (matches DayNightCycle constants).
float moonPhaseFromTime(float timeSeconds) {
    float day = floor((6000.0 + timeSeconds * 20.0) / 24000.0);
    return mod(day, 8.0);
}

void main() {
    if (camera.renderSettings.y > 0.5) {
        outColor = vec4(0.0196, 0.0196, 0.20, 1.0);
        return;
    }
    vec3 direction = normalize(worldDirection);
    float elevation = direction.y;
    float daylight = clamp((camera.sunDirection.w - 0.08) / 0.92, 0.0, 1.0);
    vec3 zenithColor = mix(nightZenithColor, dayZenithColor, daylight);
    float upperBlend = smoothstep(-0.08, 0.72, elevation);
    vec3 sky = mix(camera.horizonFog.rgb, zenithColor, upperBlend);
    float belowHorizon = 1.0 - smoothstep(-0.35, 0.08, elevation);
    sky = mix(sky, camera.horizonFog.rgb * 0.72, belowHorizon);

    vec3 sunDirection = normalize(camera.sunDirection.xyz);

    // Sun: bright during the day and matching the historical 0.145 half-height.
    sky += celestialSprite(sunDirection, sunLayer, 0.145) * (0.35 + daylight) * 1.05;

    // Moon: opposite the sun (vanilla draws it at -y=100), ~2/3 the sun size and
    // driven bright at night. The phase selects one of the eight tiles.
    float night = 1.0 - daylight;
    float moonLayer = moonFirstLayer + moonPhaseFromTime(camera.horizonFog.w);
    sky += celestialSprite(-sunDirection, moonLayer, 0.097) * (0.15 + night * 0.9);

    // Warm sun glow halo across the sky.
    float sunAlignment = max(dot(direction, sunDirection), 0.0);
    sky += vec3(1.0, 0.91, 0.68) * pow(sunAlignment, 48.0) * 0.16 * daylight;

    outColor = vec4(sky, 1.0);
}
