#version 450

layout(location = 0) in vec3 viewRay;
layout(location = 0) out vec4 outColor;

layout(binding = 5) uniform sampler2DArray panoramaTextures;

layout(push_constant) uniform PanoramaPush {
    vec4 rotationFov; // x = yaw, y = pitch (radians), z = tan(fov/2), w = aspect
    vec4 blur;        // x = background blur radius in framebuffer pixels
} pano;

const float PI = 3.14159265358979323846;

// Mirrors vanilla's CubeMap render: an 85-degree perspective view from inside a
// unit cube whose six faces carry panorama_0..panorama_5. The camera looks
// straight at panorama_0 at yaw 0 (after the vanilla 180-degree X flip that
// puts the sky on panorama_4), and slowly turns as yaw and pitch advance.
// Each face is sampled with the same (u, v) layout the vanilla quads use.
vec4 samplePanorama(vec3 ray) {
    // View ray -> cube space: the vanilla CubeMap applies RotX(180) then
    // RotX(-pitch) then RotY(-yaw), i.e. d_cube = RotY(-yaw)*RotX(-pitch)*RotX(PI)*d_view.
    vec3 d = vec3(ray.x, -ray.y, -ray.z);
    float pitch = pano.rotationFov.y;
    float cp = cos(pitch);
    float sp = sin(pitch);
    float y1 = d.y * cp + d.z * sp;   // RotX(-pitch)
    float z1 = -d.y * sp + d.z * cp;
    float yaw = pano.rotationFov.x;
    float cy = cos(yaw);
    float sy = sin(yaw);
    float x2 = d.x * cy - z1 * sy;    // RotY(-yaw)
    float z2 = d.x * sy + z1 * cy;
    vec3 dir = vec3(x2, y1, z2);

    // Intersect the unit cube along the ray and pick the face.
    float maxComponent = max(max(abs(dir.x), abs(dir.y)), abs(dir.z));
    vec3 point = dir / maxComponent;

    float layer;
    vec2 uv;
    if (abs(dir.x) >= abs(dir.y) && abs(dir.x) >= abs(dir.z)) {
        if (dir.x > 0.0) {
            layer = 1.0; // +X = panorama_1
            uv = vec2((1.0 - point.z) * 0.5, (1.0 + point.y) * 0.5);
        } else {
            layer = 3.0; // -X = panorama_3
            uv = vec2((1.0 + point.z) * 0.5, (1.0 + point.y) * 0.5);
        }
    } else if (abs(dir.y) >= abs(dir.x) && abs(dir.y) >= abs(dir.z)) {
        if (dir.y > 0.0) {
            layer = 5.0; // +Y = panorama_5
            uv = vec2((1.0 + point.x) * 0.5, (1.0 - point.z) * 0.5);
        } else {
            layer = 4.0; // -Y = panorama_4
            uv = vec2((1.0 + point.x) * 0.5, (1.0 + point.z) * 0.5);
        }
    } else if (dir.z > 0.0) {
        layer = 0.0; // +Z = panorama_0
        uv = vec2((1.0 + point.x) * 0.5, (1.0 + point.y) * 0.5);
    } else {
        layer = 2.0; // -Z = panorama_2
        uv = vec2((1.0 - point.x) * 0.5, (1.0 + point.y) * 0.5);
    }
    return texture(panoramaTextures, vec3(uv, layer));
}

void main() {
    vec4 texel;
    if (pano.blur.x < 0.5) {
        texel = samplePanorama(normalize(viewRay));
    } else {
        // Java 26.1 applies a separable box-blur post effect before drawing the
        // menu stratum. This panorama is already isolated from the sharp GUI,
        // so sample the same radius directly in view-ray space: derivatives
        // express exactly one framebuffer pixel and the 5x5 bilinear kernel
        // approximates the two-pass result without an extra render target.
        vec3 pixelX = dFdx(viewRay) * (pano.blur.x * 0.5);
        vec3 pixelY = dFdy(viewRay) * (pano.blur.x * 0.5);
        texel = vec4(0.0);
        for (int y = -2; y <= 2; ++y) {
            for (int x = -2; x <= 2; ++x) {
                vec3 ray = viewRay + pixelX * float(x) + pixelY * float(y);
                texel += samplePanorama(normalize(ray));
            }
        }
        texel *= 1.0 / 25.0;
    }
    outColor = vec4(texel.rgb, 1.0);
}
