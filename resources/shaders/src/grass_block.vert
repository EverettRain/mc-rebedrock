#version 450

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

// PackedVoxelVertex: 20 bytes, five 4-byte-aligned integer attributes.
// Positions are local to the section; the section origin arrives per-draw in a
// push constant. Normal indices address kVertexNormals below. The light
// channels are u8 (n/15 levels round-trip exactly through *17/255).
layout(push_constant) uniform TerrainPush {
    vec4 sectionOrigin;
} terrain;

layout(location = 0) in uvec2 inPosXY;  // positionX, positionY
layout(location = 1) in uvec2 inZNorm;  // positionZ, normalIndex | (pad << 8)
layout(location = 2) in uvec2 inUv;     // uvX, uvY
layout(location = 3) in uint inLayerAO; // textureLayer | (AO << 16) | (waterDepth << 24)
layout(location = 4) in uvec4 inLights; // sky, block, flatSky, flatBlock

layout(location = 0) out vec2 fragmentUv;
layout(location = 1) out vec3 fragmentNormal;
layout(location = 2) flat out float fragmentTextureLayer;
layout(location = 3) out float fragmentCameraDistance;
layout(location = 4) out float fragmentAmbientOcclusion;
layout(location = 5) out float fragmentSkyLight;
layout(location = 6) out vec3 fragmentWorldPosition;
layout(location = 7) out float fragmentBlockLight;
layout(location = 8) flat out float fragmentFlatSkyLight;
layout(location = 9) flat out float fragmentFlatBlockLight;

const float kLocalScale = 17.0 / 65535.0;
const float kUvScale = 2.0 / 65535.0;

const vec3 kVertexNormals[14] = vec3[14](
    vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
    vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0),
    vec3(0.0, 0.900552, -0.434749), vec3(0.0, -0.434749, -0.900552),
    vec3(0.434749, 0.900552, 0.0), vec3(0.900552, -0.434749, 0.0),
    vec3(0.0, 0.900552, 0.434749), vec3(0.0, -0.434749, 0.900552),
    vec3(-0.434749, 0.900552, 0.0), vec3(-0.900552, -0.434749, 0.0)
);

void main() {
    uint posZ = inZNorm.x & 0xFFFFu;
    vec3 local = vec3(float(inPosXY.x), float(inPosXY.y), float(posZ)) * kLocalScale - vec3(0.5);
    vec3 world = terrain.sectionOrigin.xyz + local;
    int normalIndex = int(inZNorm.y & 0xFFu);
    vec2 uv = vec2(inUv) * kUvScale - vec2(0.5);
    float layer = float(inLayerAO & 0xFFFFu);
    float ao = float((inLayerAO >> 16) & 0xFFu) / 255.0;
    float depth = float((inLayerAO >> 24) & 0xFFu);

    gl_Position = camera.projection * camera.view * camera.model * vec4(world, 1.0);
    fragmentUv = uv;
    fragmentNormal = normalize(mat3(camera.model) * kVertexNormals[normalIndex]);
    fragmentTextureLayer = layer;
    fragmentCameraDistance = distance(world, camera.cameraPosition.xyz);
    // Water faces carry the optical column depth in the waterDepth channel;
    // opaque faces carry AO in the ambientOcclusion channel. The fragment picks
    // one based on the layer it is rendering.
    fragmentAmbientOcclusion = (layer == 20.0 || layer == 52.0) ? depth : ao;
    fragmentSkyLight = float(inLights.x) / 255.0;
    fragmentWorldPosition = world;
    fragmentBlockLight = float(inLights.y) / 255.0;
    fragmentFlatSkyLight = float(inLights.z) / 255.0;
    fragmentFlatBlockLight = float(inLights.w) / 255.0;
}
