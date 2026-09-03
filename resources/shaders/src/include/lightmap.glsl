// 26.1's lightmap, transcribed from the vanilla shader that builds it:
// assets/minecraft/shaders/core/lightmap.fsh, with its inputs from
// LightmapRenderStateExtractor and Timelines.OVERWORLD_DAY.
//
// Vanilla bakes a 16x16 lightmap texture indexed by (blockLevel, skyLevel) and
// every terrain vertex samples it. Doing the same arithmetic per fragment costs
// a handful of instructions and skips the texture entirely, so this is the same
// formula rather than the same mechanism.
//
// The three things this replaced, each of which made the world darker than
// vanilla:
//
//   1. Sky light was capped by a `(0.72 + diffuse * 0.28)` term with no vanilla
//      counterpart. A face not pointing at the sun got 72% of the sky, so every
//      vertical surface sat at ~0.49 where vanilla has 0.6 (east/west) or 0.8
//      (north/south). Vanilla's only directional variation is CardinalLighting,
//      applied by the caller as `shade` below.
//   2. `notGamma` was missing entirely. It is the brightness option's curve and
//      it is not subtle: at sky level 10 it lifts 0.333 to 0.568.
//   3. Sky and block light were combined with max(). Vanilla ADDS them, scales
//      block light by 1.4, and whitens it toward the extremes. max() means a
//      torch can never add to daylight, which is exactly what "I put four
//      torches around the enchanting table and it is still dark" looks like.
//
// The constants are the overworld's EnvironmentAttributes defaults. They become
// per-dimension when BM-3's attribute layer carries the visual tracks; until
// then the nether and end read the overworld's, which is wrong the same way
// everything else that predates that layer is wrong.

// AMBIENT_LIGHT_COLOR default -16777216 = 0xFF000000: the overworld has no
// ambient floor. (The nether's is what makes it navigable at light 0.)
const vec3 kAmbientLightColor = vec3(0.0);
// SKY_LIGHT_COLOR default -1 = white. The day track multiplies it toward
// 0.48,0.48,1.0 at night; that tint is the caller's `skyLightColor`.
const vec3 kSkyLightColor = vec3(1.0);
// BLOCK_LIGHT_TINT default -10100 = 0xFFFFD84C.
const vec3 kBlockLightTint = vec3(1.0, 0.8470588, 0.2980392);
// LightmapRenderStateExtractor: blockFactor = blockLightFlicker + 1.4. The
// flicker is a per-tick random walk around 0; the 1.4 is the part that matters.
const float kBlockLightFactor = 1.4;
// Options.gamma default 0.5 ("options.gamma.default"). No brightness slider
// exists yet, so this is pinned at vanilla's default rather than guessed.
const float kBrightnessFactor = 0.5;

float lightmapBrightness(float level) {
    return level / (4.0 - 3.0 * level);
}

// Vanilla's `notGamma`: scales the colour so its LARGEST component follows
// 1-(1-x)^4, preserving hue. Guarded at zero — vanilla divides by the max
// component and only gets away with it because its ambient floor is non-zero in
// the dimensions it ships; the overworld's is 0, so an unlit fragment would be
// 0/0 here.
vec3 lightmapNotGamma(vec3 color) {
    float maxComponent = max(max(color.x, color.y), color.z);
    if (maxComponent <= 0.0) {
        return color;
    }
    float maxInverted = 1.0 - maxComponent;
    float maxScaled = 1.0 - maxInverted * maxInverted * maxInverted * maxInverted;
    return color * (maxScaled / maxComponent);
}

// Block light is its warm tint in the middle of its range and near-white at
// both ends — a torch's core reads white, its falloff orange.
float lightmapParabolicMixFactor(float level) {
    float centred = 2.0 * level - 1.0;
    return centred * centred;
}

// `skyLevel` and `blockLevel` are the 0..1 light levels the mesh carries.
// `skyFactor` is SKY_LIGHT_FACTOR for the current tick, times any weather
// dimming, times the sun-shadow term where the shadow pre-pass ran.
vec3 sampleLightmap(float skyLevel, float blockLevel, float skyFactor) {
    float sky = clamp(skyLevel, 0.0, 1.0);
    float block = clamp(blockLevel, 0.0, 1.0);
    float skyBrightness = lightmapBrightness(sky) * skyFactor;
    float blockBrightness = lightmapBrightness(block) * kBlockLightFactor;

    vec3 color = kAmbientLightColor;
    color += kSkyLightColor * skyBrightness;
    vec3 blockLightColor =
        mix(kBlockLightTint, vec3(1.0), 0.9 * lightmapParabolicMixFactor(block));
    color += blockLightColor * blockBrightness;

    color = clamp(color, 0.0, 1.0);
    return mix(color, lightmapNotGamma(color), kBrightnessFactor);
}

// CardinalLighting.DEFAULT: down 0.5, up 1.0, north/south 0.8, west/east 0.6.
// Vanilla picks it by face; a normal is what these shaders have, and the three
// axes are exact at the poles, so this reads the same table off |n|.
float cardinalShade(vec3 normal) {
    float vertical = normal.y > 0.0 ? 1.0 : 0.5;
    float horizontal = mix(0.6, 0.8, abs(normal.z));
    return mix(horizontal, vertical, abs(normal.y));
}
