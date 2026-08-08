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

// The per-particle records written by the CPU each frame. A single unsized
// array of a std430 struct mirrors ParticleRecord in GpuSceneBuffer.hpp (three
// aligned vec4s = 48 bytes); an SSBO block may only have one unsized member and
// it must be last.
struct ParticleRecord {
    vec4 positionSize;
    vec4 uvOriginScale;
    vec4 layerLight;
};

layout(set = 1, binding = 0) readonly buffer ParticleBuffer {
    ParticleRecord records[];
} particles;

layout(location = 0) out vec2 fragmentUv;
layout(location = 1) flat out float fragmentTextureLayer;
layout(location = 5) flat out float fragmentOpacity;
layout(location = 6) out float fragmentCameraDistance;
layout(location = 8) flat out vec2 fragmentSceneLight;
layout(location = 9) out vec3 fragmentWorldPosition;

// Camera-facing billboard corners, same winding as item_entity.vert's
// billboard path so the particles keep matching the world.
const vec2 corners[6] = vec2[](
    vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.5, 0.5),
    vec2(-0.5, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5)
);

vec2 decodeSceneLight(float packedLight) {
    if (packedLight < 0.5) {
        return vec2(-1.0);
    }
    float value = packedLight - 1.0;
    return vec2(mod(value, 16.0), floor(value / 16.0)) / 15.0;
}

void main() {
    vec4 ps = particles.records[gl_InstanceIndex].positionSize;
    vec4 uv = particles.records[gl_InstanceIndex].uvOriginScale;
    vec4 ll = particles.records[gl_InstanceIndex].layerLight;
    vec2 corner = corners[gl_VertexIndex];
    vec3 cameraRight = vec3(camera.view[0][0], camera.view[1][0], camera.view[2][0]);
    vec3 cameraUp = vec3(camera.view[0][1], camera.view[1][1], camera.view[2][1]);
    vec3 worldPosition = ps.xyz + (cameraRight * corner.x + cameraUp * corner.y) * ps.w;
    gl_Position = camera.projection * camera.view * vec4(worldPosition, 1.0);
    fragmentWorldPosition = worldPosition;
    fragmentUv = uv.xy + (corner + vec2(0.5)) * uv.z;
    fragmentTextureLayer = ll.x;
    fragmentOpacity = uv.w;
    fragmentCameraDistance = distance(worldPosition, camera.cameraPosition.xyz);
    fragmentSceneLight = decodeSceneLight(ll.y);
}
